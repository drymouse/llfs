#include "dummyfs.h"

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>

static int dummyfs_init(struct fs_context *fc);
static int init_fs_module(void);
static void cleanup_fs_module(void);
static void dummyfs_kill(struct super_block *sb);
static int dummyfs_get_tree(struct fs_context *fc);

static struct fs_context_operations fc_ope = {
    .get_tree = dummyfs_get_tree,
};

static int dummyfs_init(struct fs_context *fc) {
    pr_info("Dummy FS is mounted!\n");

    fc->ops = &fc_ope;

    return 0;
}

static void dummyfs_kill(struct super_block *sb) {
    pr_info("Dummy FS is unmounted!\n");

    return;
}

static int dummyfs_fill_super(struct super_block *sb, struct fs_context *fc) {
    struct inode *root_inode;
    struct dentry *root_dentry;

    sb->s_magic = 0xABCDEF;

    root_inode = new_inode(sb);
    if (!root_inode) {
        return -ENOMEM;
    }

    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    // root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);

    root_inode->i_op = &simple_dir_inode_operations;
    root_inode->i_fop = &simple_dir_operations;

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        return -ENOMEM;
    }

    sb->s_root = root_dentry;

    return 0;
}

static int dummyfs_get_tree(struct fs_context *fc) {
    pr_info(KERN_INFO "get_tree\n");

    return get_tree_nodev(fc, dummyfs_fill_super);
}

struct file_system_type dummyfs = {
    .name = "dummyfs",
    .fs_flags = 0,
    .kill_sb = dummyfs_kill,
    .init_fs_context = dummyfs_init,
};

static int init_fs_module(void) {
    pr_info(KERN_INFO "Dummy FS module loaded\n");

    register_filesystem(&dummyfs);

    return 0;
}

static void cleanup_fs_module(void) { pr_info(KERN_INFO "Dummy FS module cleaned up\n"); }

module_init(init_fs_module);
module_exit(cleanup_fs_module);

MODULE_DESCRIPTION("DUMMY FS");

MODULE_LICENSE("GPL");
