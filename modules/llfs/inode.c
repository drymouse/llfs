#include "llfs.h"
#include <linux/fs.h>
#include <linux/stat.h>

const struct super_operations llfs_sop = {
    .statfs = simple_statfs,
};

const struct inode_operations llfs_dir_iop = {
    .create = llfs_create,
    .lookup = llfs_lookup,
};

const struct inode_operations llfs_file_iop = {};

const struct file_operations llfs_file_fop = {
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .llseek = generic_file_llseek,
};

extern struct address_space_operations llfs_asops;

struct inode *llfs_make_inode(struct super_block *sb, umode_t mode, void *data) {
   struct inode *inode = new_inode(sb);

   if (!inode) return (struct inode *)NULL;

   inode->i_ino = get_next_ino();
   inode->i_private = data;

   if (S_ISDIR(mode)) {
       inode->i_op = &llfs_dir_iop;
       inode->i_fop = &simple_dir_operations;
       set_nlink(inode, 2);
   } else {
       inode->i_op = &llfs_file_iop;
       inode->i_fop = &llfs_file_fop;
   }

   inode->i_mapping->a_ops = &llfs_asops;

   return inode;
}

int llfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                   bool excl) {
    printk(KERN_INFO "creating a file: %pks", (void*)dentry->d_name.name);
    
    struct inode *inode = llfs_make_inode(dir->i_sb, mode, (void *)NULL);

    if (!inode) return -ENOMEM;
    
    inode_init_owner(idmap, inode, dir, mode);

    d_instantiate(dentry, inode);

    mark_inode_dirty(dir);
    mark_inode_dirty(inode);

    return 0;
}

struct dentry *llfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    pr_info(KERN_INFO "looking up...\n");
    d_add(dentry, NULL);
    return NULL;
}
