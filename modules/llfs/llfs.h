#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/iomap.h>

#ifndef NULL
#define NULL ((unsigned long)0)
#endif

#ifndef __LLFS
#define __LLFS

#define LLFS_N_BLOCKS 10

struct llfs_super_block {
    __le32 magic;
};

struct llfs_inode {
    __le16 mode;
    __le32 size;
    __le32 blocks;
    __le32 block[LLFS_N_BLOCKS];
};

struct llfs_dir_entry {
    __le32 inode;
    __le16 rec_len;
    __le16 name_len;
    char name[];
};

int llfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                bool excl);
struct dentry *llfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);

struct inode *llfs_make_inode(struct super_block *sb, umode_t mode, void *data);

#endif
