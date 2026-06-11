/*
 * journal.h - LLFS 簡易ジャーナリング(WAL / 物理 redo ログ)
 *
 * 設計の単一情報源は SPEC.md セクション 8。ここはその実装インタフェース。
 *
 * 方式:
 *   - メタデータのみをジャーナルする物理 redo ログ(ext3/jbd2 "journaled" の簡略版)。
 *   - 変更したメタデータ buffer_head を即 mark_buffer_dirty せず、トランザクションに溜め、
 *     コミット時に「ジャーナル領域へ書く → バリア → commit ブロック → 最終位置へ反映」の
 *     WAL 手順で原子的に永続化する。
 *   - 単一トランザクション(同時に1つ)。LLFS は単一スレッド前提なので循環ログ不要。
 *
 * オンディスク・ジャーナル領域(先頭 = sb.journal_block, 長さ = sb.journal_blocks = J):
 *   journal_block + 0          : descriptor(目録)
 *   journal_block + 1 .. +n    : data(変更後ブロックの丸ごとコピー)n 個
 *   journal_block + (J - 1)    : commit(★固定位置★。SPEC §8.4 の「data 直後」から
 *                                変更。固定位置にして「コミット点を毎回先に無効化」でき、
 *                                seq 再利用による torn-commit 誤検出を完全に防ぐ)
 */
#ifndef __LLFS_JOURNAL
#define __LLFS_JOURNAL

#include <linux/types.h>

struct super_block;
struct buffer_head;

/* descriptor / commit のマジック("LLJD" / "LLJC") */
#define LLFS_JRNL_DESC_MAGIC 0x4c4c4a44u
#define LLFS_JRNL_COMMIT_MAGIC 0x4c4c4a43u

/* 1トランザクションでステージできる最大ブロック数。
 * create が触るのは 3(inode bitmap / itable / dir)、書込割当でも +2 程度。十分な余裕。
 * J = 32 なら data は最大 (J - 2) = 30 まで(descriptor 1 + commit 1 を除く)。 */
#define LLFS_TXN_MAX 30

/* descriptor ブロック(journal_block に置く): このトランザクションの内容目録 */
struct llfs_jrnl_desc {
    __le32 magic;    // LLFS_JRNL_DESC_MAGIC(0 ならジャーナルは空)
    __le32 sequence; // トランザクション連番(commit と一致して初めて有効)
    __le32 n_blocks; // 後続する data ブロック数
    __le32 target[]; // 各 data ブロックの最終(in-place)ブロック番号、n_blocks 個
};

/* commit ブロック(固定位置 journal_block + journal_blocks - 1 に置く) */
struct llfs_jrnl_commit {
    __le32 magic;    // LLFS_JRNL_COMMIT_MAGIC
    __le32 sequence; // descriptor.sequence と一致 = コミット成立の証拠
};

/* インメモリのトランザクション。sbi->cur_txn に1つだけ保持する。 */
struct llfs_txn {
    struct super_block *sb;
    unsigned int n;                       // ステージ済みブロック数
    unsigned int target[LLFS_TXN_MAX];    // 各ブロックの最終ブロック番号
    struct buffer_head *bh[LLFS_TXN_MAX]; // 変更済み・未 dirty・pin 中の buffer_head
};

/*
 * 変更したメタデータ buffer_head をトランザクションに登録する。
 * 呼び出し側は sb_bread で得た bh を *brelse せずに* 渡すこと(所有権を移す)。
 * 既に同じブロックが登録済みなら重複登録を避け、余分な参照を brelse する。
 * トランザクションが無ければ遅延生成する(lazy begin)。
 *
 * → 従来の "mark_buffer_dirty(bh); brelse(bh);" を丸ごと置き換える。
 */
void llfs_txn_log(struct super_block *sb, struct buffer_head *bh);

/*
 * 現在のトランザクションを WAL 手順でコミットして永続化する(SPEC §8.5)。
 * sync_fs / アンマウント契機で呼ぶ。トランジクションが空なら何もしない。
 */
int llfs_txn_commit(struct super_block *sb);

/*
 * マウント時のリカバリ(SPEC §8.7)。コミット済みトランザクションがあれば redo し、
 * 未コミット(torn)なら破棄する。root inode を読む前に呼ぶこと。
 */
int llfs_journal_recover(struct super_block *sb);

#endif /* __LLFS_JOURNAL */
