#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/iomap.h>
#include <linux/types.h>

#ifndef NULL
#define NULL ((unsigned long)0)
#endif

#ifndef __LLFS
#define __LLFS

#define LLFS_N_BLOCKS 13
#define LLFS_NAME_MAXLEN 255
#define LLFS_ROOT_INODE 1
#define LLFS_SB_BLOCK 1
#define LLFS_BLOCK_SIZE 4096

/* llfs_dir_entry.file_type */
#define LLFS_FT_UNKNOWN 0
#define LLFS_FT_REG 1
#define LLFS_FT_DIR 2
#define llfs_get_sb_info(_sb) ((struct llfs_sb_info *)_sb->s_fs_info)
#define llfs_get_inode_info(_inode) ((struct llfs_inode_info *)_inode->i_private)

struct llfs_super_block {
    __le32 magic;              // Magic number
    __le32 block_size;         // Size of one block
    __le32 itable_block;       // Block number of inode table
    __le32 inode_bitmap_block; // Block number of inode bitmap
    __le32 block_bitmap_block; // Block number of block bitmap
    __le16 inode_size;         // Inode size (64 in llfs)
};

struct llfs_sb_info {       // In-memory: ネイティブ(CPU)エンディアンで保持
    u32 magic;              // Magic number
    u32 block_size;         // Size of one block
    u32 itable_block;       // Block number of inode table
    u32 inode_bitmap_block; // Block number of inode bitmap
    u32 block_bitmap_block; // Block number of block bitmap
    u16 inode_size;         // Inode size (64 in llfs)
};

struct llfs_inode {
    __le16 mode;                 // File mode
    __le16 uid;                  // Owner Uid
    __le32 size;                 // File size in bytes
    __le32 blocks;               // Blocks count
    __le32 block[LLFS_N_BLOCKS]; // Pointers to blocks
};

struct llfs_inode_info { // In-memory: ネイティブ(CPU)エンディアンで保持
    u32 blocks;
    u32 block[LLFS_N_BLOCKS];
};

struct llfs_dir_entry {
    __le32 inode;   // Inode number
    __le16 rec_len; // Entry length
    __u8 name_len;  // Filename length
    __u8 file_type; // File type
    char name[];    // File name
};

struct llfs_itable {
    struct llfs_inode table[];
};

int llfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
                bool excl);
struct dentry *llfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);

struct inode *llfs_make_inode(struct super_block *sb, umode_t mode, void *data);
struct inode *llfs_iget(struct super_block *sb, unsigned long ino);

#endif
