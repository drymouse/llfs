/*
 * journal.c - LLFS 簡易ジャーナリング実装(WAL / 物理 redo ログ)
 *
 * 設計: SPEC.md セクション 8。インタフェース: journal.h。
 *
 * 本ファイルが提供する不変条件(WAL):
 *   「ジャーナルの commit が永続化するまで、その操作のメタを最終位置へ書かない」
 * を、メタ更新箇所で mark_buffer_dirty を即時に呼ばず llfs_txn_log に溜め、
 * commit 時にだけ in-place へ反映することで守る。
 */
#include "llfs.h"

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>

/* --- 小さなヘルパ --- */

/* バッファを dirty 化し、I/O 完了まで待つ(1ブロックの同期書き込み) */
static int jwrite_sync(struct buffer_head *bh) {
    mark_buffer_dirty(bh);
    return sync_dirty_buffer(bh);
}

/* デバイスのライトバックキャッシュをフラッシュするバリア(QEMU/実機対策, SPEC §8.12) */
static void jbarrier(struct super_block *sb) { blkdev_issue_flush(sb->s_bdev); }

/* 現在のトランザクションを取得。無ければ遅延生成(lazy begin) */
static struct llfs_txn *llfs_txn_get(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);

    if (!sbi->cur_txn) {
        struct llfs_txn *t = kzalloc(sizeof(*t), GFP_KERNEL);
        if (!t)
            return NULL;
        t->sb = sb;
        sbi->cur_txn = t;
    }
    return sbi->cur_txn;
}

/* ロック保持を前提とする内部コミット(txn-full 再入と sync_fs/kill の両方から使う) */
static int __llfs_txn_commit(struct super_block *sb);

/* --- 公開 API --- */

/* 前提: 呼び出し側が sbi->lock を保持していること(全メタ更新点で保持済み)。 */
void llfs_txn_log(struct super_block *sb, struct buffer_head *bh) {
    struct llfs_txn *t = llfs_txn_get(sb);
    unsigned int i;

    if (!t) {
        /* ジャーナル生成に失敗 → フォールバック(従来どおり即時反映)。
         * 一貫性保証は落ちるが、メモリ不足時に操作を失わせないため。 */
        pr_warn("llfs: no transaction, falling back to direct write (blk %llu)\n",
                (unsigned long long)bh->b_blocknr);
        mark_buffer_dirty(bh);
        brelse(bh);
        return;
    }

    /* 同一ブロックの重複登録を避ける(例: itable は複数 inode 書込で何度も来る)。
     * 同じブロックは buffer cache が同じ bh を返すので、余分な参照を1つ手放す。 */
    for (i = 0; i < t->n; i++) {
        if (t->target[i] == bh->b_blocknr) {
            brelse(bh);
            return;
        }
    }

    /* オーバーフロー時は現トランザクションをコミットしてから新規に積む。
     * (LLFS_TXN_MAX に十分な余裕があり meta-loop では発生しないが安全弁) */
    if (t->n >= LLFS_TXN_MAX) {
        pr_info("llfs: txn full (%u), forcing commit\n", t->n);
        __llfs_txn_commit(sb); /* ロック保持中なので内部版を呼ぶ */
        t = llfs_txn_get(sb);
        if (!t) {
            mark_buffer_dirty(bh);
            brelse(bh);
            return;
        }
    }

    /* bh の所有権を受け取り、pin したまま(brelse せず)保持する。
     * dirty 化は commit のチェックポイントまで遅延 → カーネルが勝手に書き出さない。 */
    t->target[t->n] = bh->b_blocknr;
    t->bh[t->n] = bh;
    t->n++;
}

/* 公開コミット: ロックを取って内部版を呼ぶ。sync_fs / アンマウント契機。 */
int llfs_txn_commit(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    int ret;

    mutex_lock(&sbi->lock);
    ret = __llfs_txn_commit(sb);
    mutex_unlock(&sbi->lock);
    return ret;
}

static int __llfs_txn_commit(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    struct llfs_txn *t = sbi->cur_txn;
    struct llfs_jrnl_desc *desc;
    struct llfs_jrnl_commit *cmt;
    struct buffer_head *bh;
    unsigned int jb, J, commit_off, n, i;
    u32 seq;

    if (!t)
        return 0;
    n = t->n;
    if (n == 0) {
        sbi->cur_txn = NULL;
        kfree(t);
        return 0;
    }

    jb = sbi->journal_block;
    J = sbi->journal_blocks;
    commit_off = jb + J - 1; /* commit は固定位置(journal 末尾) */
    seq = ++sbi->journal_seq;

    pr_info("llfs: txn commit seq=%u n=%u\n", seq, n);

    /* A. 旧 commit ブロックを無効化(seq 再利用による torn-commit 誤検出を防ぐ)。
     *    これ以降クラッシュしても commit magic は無効なので「未コミット」と判定される。 */
    bh = sb_bread(sb, commit_off);
    if (!bh)
        goto fail;
    memset(bh->b_data, 0, sb->s_blocksize);
    jwrite_sync(bh);
    brelse(bh);
    jbarrier(sb);

    /* B. descriptor を書く(magic, seq, n, target[]) */
    bh = sb_bread(sb, jb);
    if (!bh)
        goto fail;
    memset(bh->b_data, 0, sb->s_blocksize);
    desc = (struct llfs_jrnl_desc *)bh->b_data;
    desc->magic = cpu_to_le32(LLFS_JRNL_DESC_MAGIC);
    desc->sequence = cpu_to_le32(seq);
    desc->n_blocks = cpu_to_le32(n);
    for (i = 0; i < n; i++)
        desc->target[i] = cpu_to_le32(t->target[i]);
    jwrite_sync(bh);
    brelse(bh);

    /* C. data ブロック(変更後のメタを丸ごとジャーナルへコピー) */
    for (i = 0; i < n; i++) {
        struct buffer_head *jbh = sb_bread(sb, jb + 1 + i);
        if (!jbh)
            goto fail;
        memcpy(jbh->b_data, t->bh[i]->b_data, sb->s_blocksize);
        jwrite_sync(jbh);
        brelse(jbh);
    }

    /* D. ── BARRIER ── descriptor + data を確実に永続化 */
    jbarrier(sb);

    /* E. commit ブロックを書く ← ★コミット点★ */
    bh = sb_bread(sb, commit_off);
    if (!bh)
        goto fail;
    cmt = (struct llfs_jrnl_commit *)bh->b_data;
    cmt->magic = cpu_to_le32(LLFS_JRNL_COMMIT_MAGIC);
    cmt->sequence = cpu_to_le32(seq);
    jwrite_sync(bh);
    brelse(bh);
    jbarrier(sb);

    /* F. チェックポイント: ここで初めて最終位置(in-place)へ反映する */
    for (i = 0; i < n; i++) {
        mark_buffer_dirty(t->bh[i]);
        sync_dirty_buffer(t->bh[i]);
    }
    jbarrier(sb);

    /* G. ジャーナルをクリア(次回マウントで replay されないように) */
    bh = sb_bread(sb, jb);
    if (!bh)
        goto fail;
    desc = (struct llfs_jrnl_desc *)bh->b_data;
    desc->magic = 0;
    jwrite_sync(bh);
    brelse(bh);
    jbarrier(sb);

    /* H. pin を解放してトランザクションを破棄 */
    for (i = 0; i < n; i++)
        brelse(t->bh[i]);
    sbi->cur_txn = NULL;
    kfree(t);
    return 0;

fail:
    /* I/O 失敗時は安全側に倒す: in-place を一切汚さず pin だけ解放する。
     * このトランザクションの変更は失われるが、ディスクは旧状態のまま一貫している。 */
    pr_err("llfs: journal commit failed, dropping transaction (seq=%u)\n", seq);
    for (i = 0; i < t->n; i++)
        brelse(t->bh[i]);
    sbi->cur_txn = NULL;
    kfree(t);
    return -EIO;
}

int llfs_journal_recover(struct super_block *sb) {
    struct llfs_sb_info *sbi = llfs_get_sb_info(sb);
    struct llfs_jrnl_desc *desc;
    struct llfs_jrnl_commit *cmt;
    struct buffer_head *dbh, *cbh;
    unsigned int jb, J, commit_off, i;
    u32 n, seq;

    jb = sbi->journal_block;
    J = sbi->journal_blocks;
    if (J == 0) {
        /* ジャーナル領域が無い(旧レイアウト)→ リカバリ不要 */
        pr_info("llfs: no journal area, skip recovery\n");
        return 0;
    }
    commit_off = jb + J - 1;

    dbh = sb_bread(sb, jb);
    if (!dbh)
        return -EIO;
    desc = (struct llfs_jrnl_desc *)dbh->b_data;

    if (le32_to_cpu(desc->magic) != LLFS_JRNL_DESC_MAGIC) {
        /* ジャーナル空 → 何もしない(正常終了の通常ケース) */
        brelse(dbh);
        return 0;
    }

    n = le32_to_cpu(desc->n_blocks);
    seq = le32_to_cpu(desc->sequence);
    if (n > LLFS_TXN_MAX) {
        /* 壊れた descriptor → 破棄 */
        pr_warn("llfs: bogus journal n_blocks=%u, discarding\n", n);
        desc->magic = 0;
        jwrite_sync(dbh);
        jbarrier(sb);
        brelse(dbh);
        return 0;
    }

    cbh = sb_bread(sb, commit_off);
    if (!cbh) {
        brelse(dbh);
        return -EIO;
    }
    cmt = (struct llfs_jrnl_commit *)cbh->b_data;

    if (le32_to_cpu(cmt->magic) != LLFS_JRNL_COMMIT_MAGIC ||
        le32_to_cpu(cmt->sequence) != seq) {
        /* 未コミット(torn write)→ 破棄。in-place は手付かずなので旧状態で一貫。 */
        pr_info("llfs: journal not committed (torn), discarding seq=%u\n", seq);
        desc->magic = 0;
        jwrite_sync(dbh);
        jbarrier(sb);
        brelse(cbh);
        brelse(dbh);
        return 0;
    }

    /* コミット済み → redo(同一内容の上書きなので冪等) */
    pr_info("llfs: replaying journal seq=%u (%u blocks)\n", seq, n);
    for (i = 0; i < n; i++) {
        u32 target = le32_to_cpu(desc->target[i]);
        struct buffer_head *src = sb_bread(sb, jb + 1 + i);
        struct buffer_head *dst;

        if (!src) {
            pr_warn("llfs: replay: cannot read journal data %u\n", i);
            continue;
        }
        dst = sb_bread(sb, target);
        if (!dst) {
            pr_warn("llfs: replay: cannot read target %u\n", target);
            brelse(src);
            continue;
        }
        memcpy(dst->b_data, src->b_data, sb->s_blocksize);
        mark_buffer_dirty(dst);
        sync_dirty_buffer(dst);
        brelse(dst);
        brelse(src);
    }
    jbarrier(sb);

    /* クリア(冪等だが二度手間を避ける) */
    desc->magic = 0;
    jwrite_sync(dbh);
    jbarrier(sb);

    brelse(cbh);
    brelse(dbh);
    return 0;
}
