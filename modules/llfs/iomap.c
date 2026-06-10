#include "llfs.h"
#include <linux/fs.h>
#include <linux/iomap.h>

static int llfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length, unsigned flags,
                            struct iomap *iomap, struct iomap *srcmap);
static int llfs_iomap_end(struct inode *inode, loff_t offset, loff_t length, ssize_t written,
                          unsigned flags, struct iomap *iomap);
static int llfs_read_folio(struct file *unused, struct folio *folio);
static void llfs_readahead(struct readahead_control *rac);
static int llfs_writepages(struct address_space *mapping, struct writeback_control *wbc);
// static int llfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos,
// unsigned len, struct folio **foliop, void **fsdata); static int llfs_write_end(const struct kiocb
// *iocb, struct address_space *mapping, loff_t pos, unsigned len, unsigned copied, struct folio
// *folio, void *fsdata);

const struct iomap_ops llfs_iomap_ops = {
    .iomap_begin = llfs_iomap_begin,
    .iomap_end = llfs_iomap_end,
};

const struct address_space_operations llfs_asops = {
    .read_folio = llfs_read_folio,
    .readahead = llfs_readahead,
    .writepages = llfs_writepages,
    // .write_begin = llfs_write_begin,
    // .write_end = llfs_write_end,
};

unsigned llfs_alloc_block(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    unsigned bitmap_block = sbi->block_bitmap_block;
    struct buffer_head *bh = sb_bread(sb, bitmap_block);
    if (!bh) {
        return 0;
    }

    long unsigned int *bitmap = (long unsigned int *)bh->b_data;

    /* find_first_zero_bit の第2引数はビット数。block_size はバイト数なので 8 倍する */
    unsigned int res = find_first_zero_bit(bitmap, sbi->block_size * 8);
    if (res == 0 || res >= LLFS_BLOCK_SIZE << 3) {
        pr_warn("unsound block nnumber\n");
        return 0;
    }
    set_bit(res, bitmap);
    mark_buffer_dirty(bh);

    brelse(bh);

    pr_info("first free block: %d\n", res);

    return res;
}

static ssize_t llfs_writeback_range(struct iomap_writepage_ctx *wpc, struct folio *folio,
                                    u64 offset, unsigned len, u64 end_pos);

const struct iomap_writeback_ops llfs_writeback_ops = {
    .writeback_range = llfs_writeback_range,
    .writeback_submit = iomap_ioend_writeback_submit, // 汎用ヘルパでOK
};

static int llfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length, unsigned flags,
                            struct iomap *iomap, struct iomap *srcmap) {
    // pr_info("llfs iomap begin...\n");

    struct llfs_inode_info *inodei = llfs_get_inode_info_checked(inode);
    if (IS_ERR(inodei)) {
        return -EIO;
    }

    unsigned sector = offset >> LLFS_BLOCK_SIZE_SHIFT;

    if (sector >= LLFS_N_BLOCKS)
        return -EIO;

    unsigned bno = inodei->block[sector];
    iomap->bdev = inode->i_sb->s_bdev;
    iomap->offset = offset;
    iomap->length = LLFS_BLOCK_SIZE;

    if (bno == 0) {
        // 割り当てされていない
        if (!(flags & IOMAP_WRITE)) {
            // 穴として報告
            iomap->addr = IOMAP_NULL_ADDR;
            iomap->type = IOMAP_HOLE;
            return 0;
        }
        pr_info("llfs iomap begin: Let's allocate block!");
        bno = llfs_alloc_block(inode->i_sb);
        if (bno == 0) {
            return -EIO;
        }

        // itableの更新
        inodei->block[sector] = bno;
        inode->i_blocks++;

        mark_inode_dirty(inode);
        iomap->flags |= IOMAP_F_NEW;
    }

    iomap->addr = bno << LLFS_BLOCK_SIZE_SHIFT;
    iomap->type = IOMAP_MAPPED;

    return 0;
}

static int llfs_iomap_end(struct inode *inode, loff_t offset, loff_t length, ssize_t written,
                          unsigned flags, struct iomap *iomap) {
    // pr_info(KERN_INFO "llfs iomap end...\n");

    if (iomap->flags & IOMAP_F_SIZE_CHANGED) {
        mark_inode_dirty(inode);
    }

    return 0;
}

static int llfs_read_folio(struct file *unused, struct folio *folio) {
    iomap_bio_read_folio(folio, &llfs_iomap_ops);
    return 0;
}

static void llfs_readahead(struct readahead_control *rac) {
    iomap_bio_readahead(rac, &llfs_iomap_ops);
}

static int llfs_writepages(struct address_space *mapping, struct writeback_control *wbc) {
    pr_info("LLFS write pages\n");
    struct iomap_writepage_ctx wpc = {
        .inode = mapping->host,
        .wbc = wbc,
        .ops = &llfs_writeback_ops,
    };

    return iomap_writepages(&wpc);
}

static ssize_t llfs_writeback_range(struct iomap_writepage_ctx *wpc, struct folio *folio,
                                    u64 offset, unsigned len, u64 end_pos) {
    pr_info("llfs writeback range\n");

    if (offset < wpc->iomap.offset || offset >= wpc->iomap.offset + wpc->iomap.length) {
        pr_info("need to do something...\n");

        int error =
            llfs_iomap_begin(wpc->inode, offset, INT_MAX, IOMAP_WRITE, &wpc->iomap, NULL);
        if (error)
            return error;
    }

    return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}
