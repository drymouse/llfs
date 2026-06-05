#include "dummyfs.h"
#include <linux/fs.h>
#include <linux/stat.h>

const struct super_operations dummyfs_sop = {
    .statfs = simple_statfs,
};

const struct inode_operations dummyfs_dir_iop = {
    .create = dummyfs_create,
    // .lookup = simple_lookup,
    .lookup = dummyfs_lookup,
};

const struct inode_operations dummyfs_file_iop = {};

const struct file_operations dummyfs_file_fop = {
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .llseek = generic_file_llseek,
};

// static unsigned int ino_top = 2;

// unsigned int get_next_ino() {
//     return ino_top++;
// }

struct inode *dummyfs_make_inode(struct super_block *sb, umode_t mode, void *data) {
   struct inode *inode = new_inode(sb);

   if (!inode) return (struct inode *)NULL;

   inode->i_ino = get_next_ino();
   inode->i_private = data;

   if (S_ISDIR(mode)) {
       inode->i_op = &dummyfs_dir_iop;
       inode->i_fop = &simple_dir_operations;
       set_nlink(inode, 2);
   } else {
       inode->i_op = &dummyfs_file_iop;
       inode->i_fop = &dummyfs_file_fop;
   }

   return inode;
}

int dummyfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                   bool excl) {
    printk(KERN_INFO "creating a file: %pks", (void*)dentry->d_name.name);
    
    struct inode *inode = dummyfs_make_inode(dir->i_sb, mode, (void *)NULL);

    if (!inode) return -ENOMEM;
    
    inode_init_owner(idmap, inode, dir, mode);

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
