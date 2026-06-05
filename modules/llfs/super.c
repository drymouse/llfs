#include "llfs.h"

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/printk.h>
#include <linux/slab.h>

static int llfs_get_tree(struct fs_context *fc);
extern struct inode_operations llfs_dir_iop;
extern struct file_operations llfs_file_fop;

static struct fs_context_operations fc_ope = {
    .get_tree = llfs_get_tree,
};

extern struct address_space_operations llfs_asops;

static int llfs_init(struct fs_context *fc) {
    pr_info("LLFS is mounted!\n");

    fc->ops = &fc_ope;

    return 0;
}

static void llfs_kill(struct super_block *sb) {
    pr_info("LLFS is unmounted!\n");

    return;
}

static int llfs_fill_super(struct super_block *sb, struct fs_context *fc) {
    struct inode *root_inode;
    struct dentry *root_dentry;

    sb_set_blocksize(sb, 4096);

    struct buffer_head *bh;
    bh = sb_bread(sb, 1);
    if (!bh) {
        pr_info(KERN_INFO "sb_bread failed\n");
        return -ENODEV;
    }

    sb->s_magic = (unsigned long)((struct llfs_super_block *)bh->b_data)->magic;

    pr_info(KERN_INFO "magic: 0x%08lx\n", sb->s_magic);

    root_inode = new_inode(sb);
    if (!root_inode) {
        return -ENOMEM;
    }

    root_inode->i_ino = get_next_ino();
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_mapping->a_ops = &llfs_asops;
    // root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);

    // root_inode->i_op = &simple_dir_inode_operations;
    root_inode->i_op = &llfs_dir_iop;
    root_inode->i_fop = &simple_dir_operations;

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        return -ENOMEM;
    }

    sb->s_root = root_dentry;

    brelse(bh);

    return 0;
}

static int llfs_get_tree(struct fs_context *fc) {
    pr_info(KERN_INFO "get_tree\n");

    return get_tree_bdev(fc, llfs_fill_super);
}

struct file_system_type llfs = {
    .name = "llfs",
    .fs_flags = 0,
    .kill_sb = llfs_kill,
    .init_fs_context = llfs_init,
};

static int init_fs_module(void) {
    pr_info(KERN_INFO "LLFS module loaded\n");

    register_filesystem(&llfs);

    return 0;
}

static void cleanup_fs_module(void) { pr_info(KERN_INFO "LLFS module cleaned up\n"); }

module_init(init_fs_module);
module_exit(cleanup_fs_module);

MODULE_DESCRIPTION("LLUUIIGGEE FS");

MODULE_LICENSE("GPL");
