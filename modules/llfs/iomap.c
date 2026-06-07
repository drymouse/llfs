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
// static int llfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
// static int llfs_write_end(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, unsigned copied, struct folio *folio, void *fsdata);

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

const struct iomap_writeback_ops llfs_writeback_ops = {};

static int llfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length, unsigned flags,
                            struct iomap *iomap, struct iomap *srcmap) {
    pr_info(KERN_INFO "llfs iomap begin...\n");
    
    iomap->bdev = inode->i_sb->s_bdev;
    iomap->addr = 0x800;
    iomap->offset = offset;
    iomap->length = 0x400;
    iomap->type = IOMAP_MAPPED;
    
    return 0;
}

static int llfs_iomap_end(struct inode *inode, loff_t offset, loff_t length, ssize_t written,
                          unsigned flags, struct iomap *iomap) {
    pr_info(KERN_INFO "llfs iomap end...\n");

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
    pr_info(KERN_INFO "LLFS write pages\n");
    struct iomap_writepage_ctx wpc = {
        .inode = mapping->host,
        .wbc = wbc,
        .ops = &llfs_writeback_ops,
    };

    return iomap_writepages(&wpc);
}

