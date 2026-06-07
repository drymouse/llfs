#include "llfs.h"

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/printk.h>
#include <linux/slab.h>

static int llfs_get_tree(struct fs_context *fc);
extern struct inode_operations llfs_dir_iop;
extern struct file_operations llfs_file_fop;
extern const struct super_operations llfs_sop;

static struct fs_context_operations fc_ope = {
    .get_tree = llfs_get_tree,
};

extern struct address_space_operations llfs_asops;

static int llfs_init(struct fs_context *fc) {
    pr_info("LLFS is mounted!\n");

    fc->ops = &fc_ope;

    return 0;
}

static void llfs_kill(struct super_block *sb) {
    pr_info("LLFS is unmounted!\n");

    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;

    kill_block_super(sb);
}

static int llfs_fill_super(struct super_block *sb, struct fs_context *fc) {
    struct inode *root_inode;
    struct dentry *root_dentry;
    struct buffer_head *bh;
    struct llfs_super_block *dsb;
    struct llfs_sb_info *sbi;

    sb_set_blocksize(sb, LLFS_BLOCK_SIZE);

    bh = sb_bread(sb, LLFS_SB_BLOCK);
    if (!bh) {
        pr_info(KERN_INFO "sb_bread failed\n");
        return -ENODEV;
    }

    /* オンディスクのスーパーブロックを、インメモリの sb_info へ
     * 1度だけエンディアン変換して取り込む。以降は CPU ネイティブ型で扱う。 */
    dsb = (struct llfs_super_block *)bh->b_data;

    sbi = kzalloc_obj(*sbi);
    if (!sbi) {
        brelse(bh);
        return -ENOMEM;
    }

    sbi->magic              = le32_to_cpu(dsb->magic);
    sbi->block_size         = le32_to_cpu(dsb->block_size);
    sbi->itable_block       = le32_to_cpu(dsb->itable_block);
    sbi->inode_bitmap_block = le32_to_cpu(dsb->inode_bitmap_block);
    sbi->block_bitmap_block = le32_to_cpu(dsb->block_bitmap_block);
    sbi->inode_size         = le16_to_cpu(dsb->inode_size);

    sb->s_fs_info = sbi;
    sb->s_magic   = sbi->magic;
    sb->s_op      = &llfs_sop;

    /* 取り込みが済んだら、オンディスクバッファはもう不要 */
    brelse(bh);

    pr_info(KERN_INFO "magic: 0x%08lx\n", sb->s_magic);

    if (sbi->block_size != LLFS_BLOCK_SIZE) {
        pr_info(KERN_INFO "unexpected block_size: %u\n", sbi->block_size);
        return -EINVAL;
    }

    /* ルート inode は inode テーブルから読む(i_op/i_fop/a_ops/i_private は iget が設定) */
    root_inode = llfs_iget(sb, LLFS_ROOT_INODE);
    if (IS_ERR(root_inode)) {
        return PTR_ERR(root_inode);
    }

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        return -ENOMEM;
    }

    sb->s_root = root_dentry;

    return 0;
}

static int llfs_get_tree(struct fs_context *fc) {
    pr_info(KERN_INFO "get_tree\n");

    return get_tree_bdev(fc, llfs_fill_super);
}

struct file_system_type llfs = {
    .name = "llfs",
    .fs_flags = 0,
    .kill_sb = llfs_kill,
    .init_fs_context = llfs_init,
};

static int init_fs_module(void) {
    pr_info(KERN_INFO "LLFS module loaded\n");

    register_filesystem(&llfs);

    return 0;
}

static void cleanup_fs_module(void) { pr_info(KERN_INFO "LLFS module cleaned up\n"); }

module_init(init_fs_module);
module_exit(cleanup_fs_module);

MODULE_DESCRIPTION("LLUUIIGGEE FS");

MODULE_LICENSE("GPL");
