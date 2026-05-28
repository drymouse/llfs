#include "dummyfs.h"

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/printk.h>

struct fs_context_operations fc_ope = {
    .get_tree = dummyfs_get_tree,
};

struct super_operations dummyfs_sop = {};

struct inode_operations dummyfs_iop = {};

struct file_operations dummyfs_fop = {};

struct dentry_operations dummyfs_dop = {};

int dummyfs_init(struct fs_context *fc) {
    pr_info("Dummy FS is mounted!\n");

    fc->ops = &fc_ope;

    return 0;
}

void dummyfs_kill(struct super_block *sb) {
    pr_info("Dummy FS is unmounted!\n");

    return;
}

int dummyfs_get_tree(struct fs_context *fc) {
    pr_info(KERN_INFO "get_tree\n");
    
    while (1) {
        __asm__ volatile("hlt");
    }
    return 0;
}

struct file_system_type dummyfs = {
    .name = "dummyfs",
    .fs_flags = 0,
    .kill_sb = dummyfs_kill,
    .init_fs_context = dummyfs_init,
};

int init_fs_module(void) {
    pr_info(KERN_INFO "Dummy FS module loaded\n");

    register_filesystem(&dummyfs);

    return 0;
}

void cleanup_fs_module(void) { pr_info(KERN_INFO "Dummy FS module cleaned up\n"); }

module_init(init_fs_module);
module_exit(cleanup_fs_module);

MODULE_DESCRIPTION("DUMMY FS");

MODULE_LICENSE("GPL");
