#include "dummyfs.h"
#include <linux/fs.h>

const struct super_operations dummyfs_sop = {
    .statfs = simple_statfs,
};

const struct inode_operations dummyfs_dir_iop = {
    .create = dummyfs_create,
    // .lookup = simple_lookup,
    .lookup = dummyfs_lookup,
};

const struct inode_operations dummyfs_file_iop = {};

const struct file_operations dummyfs_file_fop = {};

extern int ino_top;

int dummyfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                   bool excl) {
    struct inode *inode = new_inode(dir->i_sb);
    if (!inode) {
        return -ENOMEM;
    }

    inode->i_ino = ino_top;
    ino_top++;

    inode_init_owner(idmap, inode, dir, mode);

    inode->i_op = &dummyfs_file_iop;
    inode->i_fop = &dummyfs_file_fop;

    d_instantiate(dentry, inode);

    mark_inode_dirty(dir);
    mark_inode_dirty(inode);

    return 0;
}

struct dentry *dummyfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    pr_info(KERN_INFO "looking up...\n");
    d_add(dentry, NULL);
    return NULL;
}
