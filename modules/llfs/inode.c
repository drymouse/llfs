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

    /* inode テーブルは 1 ブロックに収まる前提なので、確保できる inode 数は
     * block_size / inode_size(= 4096/64 = 64)で頭打ち。inode bitmap は 32768 ビット
     * あるが、その上限を超える ino を返すと write_inode が itable ブロック外
     * (= 隣接する bitmap ブロック)へ書き込み、メタデータを破壊してしまう。
     * したがって探索範囲・判定ともに max_inodes で制限する。 */
    unsigned int max_inodes = sbi->block_size / sbi->inode_size;
    unsigned int res = find_first_zero_bit(bitmap, max_inodes);
    if (res == 0 || res >= max_inodes) {
        pr_warn("llfs: inode table full (max %u)\n", max_inodes);
        brelse(bh);
        return 0;
    }
    set_bit(res, bitmap);
    /* [JOURNAL] 旧: mark_buffer_dirty(bh); brelse(bh);
     * 変更したメタ(inode bitmap)を即時反映せず、トランザクションに登録する。
     * bh は brelse せずに所有権を txn へ渡す。 */
    llfs_txn_log(sb, bh);

    pr_info(KERN_INFO "first free node: %d\n", res);

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

struct llfs_inode_info *llfs_get_inode_info_checked(struct inode *inode) {
    struct llfs_inode_info *inodei = llfs_get_inode_info(inode);

    if (!inodei) {
        inodei = kzalloc_obj(*inodei);
        if (!inodei) {
            return ERR_PTR(-ENOMEM);
        }
        if (llfs_fill_inode_info(inode, inodei) < 0) {
            kfree(inodei);
            return ERR_PTR(-EIO);
        }
        inode->i_private = inodei;
    }

    return inodei;
}

int llfs_get_block(struct inode *inode, unsigned int sector, struct buffer_head **bh) {
    struct llfs_inode_info *inodei = llfs_get_inode_info_checked(inode);

    if (IS_ERR(inodei))
        return -1;

    if (sector >= LLFS_N_BLOCKS) {
        return -1;
    }

    *bh = sb_bread(inode->i_sb, inodei->block[sector]);

    if (!*bh) {
        return -1;
    }

    return 0;
}

struct llfs_itable *llfs_get_itable(struct super_block *sb, struct buffer_head **bh) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    *bh = sb_bread(sb, sbi->itable_block);
    if (!*bh)
        return ERR_PTR(-EIO);

    return (struct llfs_itable *)(*bh)->b_data;
}

static int llfs_add_dirent(struct inode *dir, struct dentry *dentry, struct inode *inode) {
    struct buffer_head *bh;
    if (llfs_get_block(dir, 0, &bh)) {
        return -1;
    }

    unsigned name_len = strlen(dentry->d_name.name);

    if (name_len > LLFS_NAME_MAXLEN) {
        brelse(bh);
        return -1;
    }

    char *ptr = bh->b_data;
    struct llfs_dir_entry *dirent;

    while (1) {
        dirent = (struct llfs_dir_entry *)ptr;

        if (dirent->inode == 0) {
            break;
        }

        ptr += le16_to_cpu(dirent->rec_len);
    }

    dirent->inode = cpu_to_le32(inode->i_ino);
    dirent->name_len = (u8)name_len;
    dirent->file_type = LLFS_FT_REG; // TODO
    dirent->rec_len = cpu_to_le16(((name_len >> 2) << 2) + 4 + 8);
    memcpy(dirent->name, dentry->d_name.name, name_len);

    /* [JOURNAL] 旧: mark_buffer_dirty(bh); brelse(bh);
     * ディレクトリデータ(dirent)の変更をトランザクションへ登録(brelse しない)。 */
    llfs_txn_log(dir->i_sb, bh);

    return 0;
}

static ssize_t llfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

static void llfs_evict_inode(struct inode *inode) {
    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);
    kfree(inode->i_private);
    inode->i_private = NULL;
}

static int llfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(inode->i_sb);
    pr_info("llfs write inode: %lu\n", inode->i_ino);

    struct buffer_head *bh;
    struct llfs_inode *dinode;
    struct llfs_inode_info *inodei;
    struct llfs_itable *itable;

    /* [JOURNAL] itable の読み出し〜直列化〜txn 登録を、iomap_begin の確保区間や
     * commit と相互排他にする。これがないと flusher の write_inode が確保途中の
     * block[] を直列化し、bitmap と itable が食い違ってしまう。 */
    mutex_lock(&sbi->lock);

    itable = llfs_get_itable(inode->i_sb, &bh);
    if (IS_ERR(itable)) {
        mutex_unlock(&sbi->lock);
        return -EIO;
    }

    dinode = &itable->table[inode->i_ino];
    inodei = llfs_get_inode_info_checked(inode);
    if (IS_ERR(inodei)) {
        brelse(bh);
        mutex_unlock(&sbi->lock);
        return PTR_ERR(inodei);
    }

    dinode->mode = cpu_to_le16(inode->i_mode);
    dinode->uid = cpu_to_le16(inode->i_uid.val);
    dinode->size = cpu_to_le32(inode->i_size);
    dinode->blocks = cpu_to_le32(inode->i_blocks);
    memcpy(dinode->block, inodei->block, sizeof(dinode->block));

    /* [JOURNAL] 旧: mark_buffer_dirty(bh); brelse(bh);
     * itable の変更をトランザクションへ登録(同一ブロックは txn 側で重複排除)。 */
    llfs_txn_log(inode->i_sb, bh);
    mutex_unlock(&sbi->lock);
    return 0;
}

/* [JOURNAL] sync(2)/sync_fs 契機で、溜まったメタ更新を1トランザクションとして
 * 原子的にコミットする。sync() は writeback(writepages/write_inode で txn_log)→
 * sync_fs の順で動くため、その操作群が1つの atomic な単位として永続化される。 */
static int llfs_sync_fs(struct super_block *sb, int wait) {
    pr_info("llfs sync_fs (wait=%d)\n", wait);
    return llfs_txn_commit(sb);
}

const struct super_operations llfs_sop = {
    .statfs = simple_statfs,
    .evict_inode = llfs_evict_inode,
    .write_inode = llfs_write_inode,
    .sync_fs = llfs_sync_fs,
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
        return ERR_PTR(-ENOMEM);

    unsigned int ino = llfs_get_first_free_inode(sb);
    if (!ino) {
        iput(inode); /* new_inode で確保した VFS inode を解放(リーク防止) */
        return ERR_PTR(-ENOSPC);
    }

    inode->i_ino = ino;
    inode->i_private = data;

    /* new_inode() は hash 登録をしないため、ここで登録する。
     * 未登録だと __mark_inode_dirty が dirty list に載せず write_inode が呼ばれない */
    insert_inode_hash(inode);

    if (S_ISDIR(mode)) {
        inode->i_op = &llfs_dir_iop;
        inode->i_fop = &simple_dir_operations;
        set_nlink(inode, 2);
    } else {
        inode->i_op = &llfs_file_iop;
        inode->i_fop = &llfs_file_fop;
        set_nlink(inode, 1);
    }

    inode->i_blocks = 0;
    inode->i_size = 0;
    inode->i_mapping->a_ops = &llfs_asops;

    return inode;
}

int llfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                bool excl) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(dir->i_sb);
    struct inode *inode;
    int err;

    printk("creating a file: %pks", (void *)dentry->d_name.name);

    /* [JOURNAL] inode bitmap 確保 + dirent 追加 + itable 直列化(後続の write_inode)が
     * 1 トランザクションに収まるよう、メタ更新区間を lock で直列化する。
     * commit(sync_fs)も同じ lock を取るため、確保途中の状態がコミットされない。 */
    mutex_lock(&sbi->lock);

    inode = llfs_make_inode(dir->i_sb, mode, (void *)NULL);
    if (IS_ERR(inode)) { /* 旧: if(!inode) は ERR_PTR を素通しする誤り */
        mutex_unlock(&sbi->lock);
        return PTR_ERR(inode);
    }

    inode_init_owner(idmap, inode, dir, mode);

    // dirに登録
    err = llfs_add_dirent(dir, dentry, inode);
    if (err) {
        /* dirent を書けなければ inode を作っても参照できない。確保した inode を捨てる。
         * (inode bitmap のビットは未コミットの txn 内なので brelse 時に in-place 未反映) */
        mutex_unlock(&sbi->lock);
        iput(inode);
        return err;
    }

    mark_inode_dirty(dir);
    mark_inode_dirty(inode);

    mutex_unlock(&sbi->lock);

    d_instantiate(dentry, inode);

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
