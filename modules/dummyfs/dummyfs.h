#include <linux/fs.h>
#include <linux/fs_context.h>

#ifndef NULL
#define NULL ((unsigned long)0)
#endif

#ifndef __DUMMYFS
#define __DUMMYFS

int dummyfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *dummyfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);

struct inode *dummyfs_make_inode(struct super_block *sb, umode_t mode, void *data);

#endif
