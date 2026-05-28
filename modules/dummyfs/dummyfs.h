#include <linux/fs.h>
#include <linux/fs_context.h>

#ifndef NULL
#define NULL ((unsigned long)0)
#endif

int dummyfs_init(struct fs_context *fc);
int init_fs_module(void);
void cleanup_fs_module(void);
void dummyfs_kill(struct super_block *);

int dummyfs_get_tree(struct fs_context *);
