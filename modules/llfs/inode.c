#include "llfs.h"
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/stat.h>

static unsigned int llfs_get_first_free_inode(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    unsigned int bitmap_block = sbi->inode_bitmap_block;
    struct buffer_head *bh = sb_bread(sb, bitmap_block);
    if (!bh) {
        return 0;
    }

    long unsigned int *bitmap = (long unsigned int *)bh->b_data;

    unsigned int res = find_first_zero_bit(bitmap, sbi->block_size);
    set_bit(res, bitmap);
    mark_buffer_dirty(bh);

    brelse(bh);

    return res;
}

static int llfs_fill_inode_info(struct inode *inode, struct llfs_inode_info *inodei) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(inode->i_sb);
    struct buffer_head *bh = sb_bread(inode->i_sb, sbi->itable_block);
    if (!bh) {
        return -1;
    }
    
    struct llfs_itable *itable = (struct llfs_itable *)bh->b_data;

    inodei->blocks = itable->table[inode->i_ino].blocks;
    memcpy(inodei->block, itable->table[inode->i_ino].block, sizeof(*inodei->block));

    brelse(bh);

    return 0;
}

static int llfs_get_block(struct inode *inode, unsigned int sector, struct buffer_head **bh) {
    struct llfs_inode_info *inodei = llfs_get_inode_info(inode);

    if (!inodei) {
        pr_info(KERN_INFO "Let's make inode_info!\n");
        inodei = kzalloc_obj(*inodei);
        if (!llfs_fill_inode_info(inode, inodei)) {
            return -1;
        }
    }

    if (sector >= LLFS_N_BLOCKS) {
        return -1;
    }

    *bh = sb_bread(inode->i_sb, inodei->block[sector]);

    if (!*bh) {
        return -1;
    }

    return 0;
}

static struct inode *llfs_fill_inode(struct super_block *sb, unsigned int ino);
static ssize_t llfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

const struct super_operations llfs_sop = {
    .statfs = simple_statfs,
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

struct dentry *llfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    pr_info(KERN_INFO "looking up...\n");

    struct buffer_head *bh;

    if (!llfs_get_block(dir, 0, &bh)) {
        return ERR_PTR(-EIO);
    }

    char *ptr = bh->b_data;

    while (1) {
        struct llfs_dir_entry *dent = (struct llfs_dir_entry *)ptr;
        if (dent->inode == 0) {
            break;
        }

        pr_info(KERN_INFO "looking up: \"%pks\"\n", dent->name);

        if (!strncmp(dent->name, dentry->d_name.name, dent->name_len)) {
            struct inode *inode = llfs_fill_inode(dir->i_sb, dent->inode);
            d_add(dentry, inode);
            return NULL;
        }

        ptr += dent->rec_len;
    }

    pr_info(KERN_INFO "look up: not found\n");
    d_add(dentry, NULL);
    return NULL;
}

static struct inode *llfs_fill_inode(struct super_block *sb, unsigned int ino) {
    struct inode *inode = new_inode(sb);
    if (!inode)
        return NULL;

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_mode = S_IFREG | 0644;
    inode->i_size = 10;
    inode->i_op = &llfs_file_iop;
    inode->i_fop = &llfs_file_fop;
    inode->i_mapping->a_ops = &llfs_asops;

    return inode;
}

static ssize_t llfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from) {
    ssize_t res = iomap_file_buffered_write(iocb, from, &llfs_iomap_ops, &llfs_iomap_wops, NULL);

    if (res > 0) {
        res = generic_write_sync(iocb, res);
    }

    return res;
}
