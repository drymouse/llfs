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
static bool dummyfs_trylock(const struct dentry *dent);

static struct fs_context_operations fc_ope = {
    .get_tree = dummyfs_get_tree,
};

static const struct super_operations dummyfs_sop = {};

static const struct inode_operations dummyfs_iop = {};

static const struct file_operations dummyfs_fop = {};

static const struct dentry_operations dummyfs_dop = {
    .d_unalias_trylock = dummyfs_trylock,
};

static struct super_block dummyfs_sb = {
    .s_op = &dummyfs_sop,
};

static struct inode root_inode = {
    .i_ino = 1,
    .i_op = &dummyfs_iop,
    .i_fop = &dummyfs_fop,
};

static struct dentry root_dent = {
    .d_parent = &root_dent,
    .d_name = {
        .name = "/",
    },
    .d_op = &dummyfs_dop,
    .d_sb = &dummyfs_sb,
    .d_inode = &root_inode,
}; 

static bool dummyfs_trylock(const struct dentry *dent) {
    pr_info(KERN_INFO "trylock\n");
    return 1;
}

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
    sb->s_root = &root_dent;

    return 0;
}

static int dummyfs_get_tree(struct fs_context *fc) {
    pr_info(KERN_INFO "get_tree\n");

    // struct inode *root_inode = kzalloc(sizeof(struct inode), GFP_KERNEL);

    
    // while (1) {
    //     __asm__ volatile("hlt");
    // }
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
