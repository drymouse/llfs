#include "llfs.h"
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/stat.h>

static unsigned int llfs_get_first_free_inode(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    unsigned int bitmap_block = sbi->inode_bitmap_block;
    struct buffer_head *bh = sb_bread(sb, bitmap_block);
    if (!bh) {
        return 0;
    }

    long unsigned int *bitmap = (long unsigned int *)bh->b_data;

    /* find_first_zero_bit の第2引数はビット数。block_size はバイト数なので 8 倍する */
    unsigned int res = find_first_zero_bit(bitmap, sbi->block_size * 8);
    set_bit(res, bitmap);
    mark_buffer_dirty(bh);

    brelse(bh);

    return res;
}

/* inode テーブルからオンディスク inode を読み、ネイティブ型へ変換して inodei に格納 */
static int llfs_fill_inode_info(struct inode *inode, struct llfs_inode_info *inodei) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(inode->i_sb);
    struct buffer_head *bh = sb_bread(inode->i_sb, sbi->itable_block);
    struct llfs_inode *raw;
    int i;

    if (!bh) {
        return -1;
    }

    raw = (struct llfs_inode *)(bh->b_data + inode->i_ino * sbi->inode_size);

    inodei->blocks = le32_to_cpu(raw->blocks);
    for (i = 0; i < LLFS_N_BLOCKS; i++) {
        inodei->block[i] = le32_to_cpu(raw->block[i]);
    }

    brelse(bh);

    return 0;
}

static int __maybe_unused llfs_get_block(struct inode *inode, unsigned int sector,
                                          struct buffer_head **bh) {
    struct llfs_inode_info *inodei = llfs_get_inode_info(inode);

    if (sector >= LLFS_N_BLOCKS) {
        return -1;
    }

    if (!inodei) {
        inodei = kzalloc_obj(*inodei);
        if (!inodei) {
            return -1;
        }
        if (llfs_fill_inode_info(inode, inodei) < 0) {
            kfree(inodei);
            return -1;
        }
        inode->i_private = inodei;
    }

    *bh = sb_bread(inode->i_sb, inodei->block[sector]);

    if (!*bh) {
        return -1;
    }

    return 0;
}

static ssize_t llfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

static void llfs_evict_inode(struct inode *inode) {
    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);
    kfree(inode->i_private);
    inode->i_private = NULL;
}

const struct super_operations llfs_sop = {
    .statfs = simple_statfs,
    .evict_inode = llfs_evict_inode,
};

const struct inode_operations llfs_dir_iop = {
    .create = llfs_create,
    .lookup = llfs_lookup,
};

const struct inode_operations llfs_file_iop = {};

const struct iomap_write_ops llfs_iomap_wops = {};

const struct file_operations llfs_file_fop = {
    .read_iter = generic_file_read_iter,
    .write_iter = llfs_file_write_iter,
    .mmap = generic_file_mmap,
    .llseek = generic_file_llseek,
};

extern struct address_space_operations llfs_asops;
extern struct iomap_ops llfs_iomap_ops;

struct inode *llfs_make_inode(struct super_block *sb, umode_t mode, void *data) {
    struct inode *inode = new_inode(sb);

    if (!inode)
        return (struct inode *)NULL;

    unsigned int ino = llfs_get_first_free_inode(sb);
    if (!ino)
        return (struct inode *)NULL;

    inode->i_ino = ino;
    inode->i_private = data;

    if (S_ISDIR(mode)) {
        inode->i_op = &llfs_dir_iop;
        inode->i_fop = &simple_dir_operations;
        set_nlink(inode, 2);
    } else {
        inode->i_op = &llfs_file_iop;
        inode->i_fop = &llfs_file_fop;
        inode->i_size = 0;
    }

    inode->i_mapping->a_ops = &llfs_asops;

    return inode;
}

int llfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                bool excl) {
    printk(KERN_INFO "creating a file: %pks", (void *)dentry->d_name.name);

    struct inode *inode = llfs_make_inode(dir->i_sb, mode, (void *)NULL);

    if (!inode)
        return -ENOMEM;

    inode_init_owner(idmap, inode, dir, mode);

    d_instantiate(dentry, inode);

    mark_inode_dirty(dir);
    mark_inode_dirty(inode);

    return 0;
}

/* inode テーブルからオンディスク inode を読み、VFS inode を構築して返す。
 * iget_locked で inode キャッシュを使い、同一 ino の重複生成を防ぐ。 */
struct inode *llfs_iget(struct super_block *sb, unsigned long ino) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    struct buffer_head *bh;
    struct llfs_inode *raw;
    struct llfs_inode_info *inodei;
    struct inode *inode;
    int i;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode_state_read(inode) & I_NEW)) /* 既にキャッシュ済み */
        return inode;

    bh = sb_bread(sb, sbi->itable_block);
    if (!bh) {
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }
    raw = (struct llfs_inode *)(bh->b_data + ino * sbi->inode_size);

    inodei = kzalloc_obj(*inodei);
    if (!inodei) {
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-ENOMEM);
    }

    inode->i_mode = le16_to_cpu(raw->mode);
    i_uid_write(inode, le16_to_cpu(raw->uid));
    i_gid_write(inode, 0);
    inode->i_size = le32_to_cpu(raw->size);

    inodei->blocks = le32_to_cpu(raw->blocks);
    for (i = 0; i < LLFS_N_BLOCKS; i++)
        inodei->block[i] = le32_to_cpu(raw->block[i]);
    inode->i_private = inodei;

    inode->i_mapping->a_ops = &llfs_asops;
    if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &llfs_dir_iop;
        inode->i_fop = &simple_dir_operations;
        set_nlink(inode, 2);
    } else {
        inode->i_op = &llfs_file_iop;
        inode->i_fop = &llfs_file_fop;
        set_nlink(inode, 1);
    }

    brelse(bh);
    unlock_new_inode(inode);
    return inode;
}

struct dentry *llfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    struct super_block *sb = dir->i_sb;
    struct llfs_inode_info *diri = llfs_get_inode_info(dir);
    struct inode *inode = NULL;
    unsigned long ino = 0;
    unsigned int b;

    if (dentry->d_name.len > LLFS_NAME_MAXLEN)
        return ERR_PTR(-ENAMETOOLONG);
    if (!diri) /* ディレクトリの inode_info が無い = 異常 */
        return ERR_PTR(-EIO);

    /* ディレクトリの各データブロックを走査し、名前が一致する inode 番号を探す */
    for (b = 0; b < diri->blocks && b < LLFS_N_BLOCKS && !ino; b++) {
        struct buffer_head *bh;
        char *p, *end;

        if (!diri->block[b])
            continue;
        bh = sb_bread(sb, diri->block[b]);
        if (!bh)
            continue;

        p = bh->b_data;
        end = p + sb->s_blocksize;
        while (p + sizeof(struct llfs_dir_entry) <= end) {
            struct llfs_dir_entry *de = (struct llfs_dir_entry *)p;
            u16 rec_len = le16_to_cpu(de->rec_len);

            if (rec_len < sizeof(struct llfs_dir_entry)) /* 壊れている */
                break;
            if (de->inode && de->name_len == dentry->d_name.len &&
                memcmp(de->name, dentry->d_name.name, de->name_len) == 0) {
                ino = le32_to_cpu(de->inode);
                break;
            }
            p += rec_len;
        }
        brelse(bh);
    }

    if (ino) {
        inode = llfs_iget(sb, ino);
        if (IS_ERR(inode))
            return ERR_CAST(inode);
    }

    /* inode==NULL なら負の dentry を作る(= not found) */
    return d_splice_alias(inode, dentry);
}

static ssize_t llfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from) {
    ssize_t res = iomap_file_buffered_write(iocb, from, &llfs_iomap_ops, &llfs_iomap_wops, NULL);

    if (res > 0) {
        res = generic_write_sync(iocb, res);
    }

    return res;
}
