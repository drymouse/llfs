/*
 * meta-loop: メタデータを激しく更新し続ける負荷プログラム。
 *
 * 目的: 突然の電源断(QEMU 強制終了 / panic)が「書き込み途中」に入ると、
 *       現状の llfs では複数のメタデータブロック(inode bitmap / block bitmap /
 *       inode テーブル / ディレクトリエントリ)が *非原子的* に更新されるため、
 *       再マウント後にこれらが互いに矛盾する(= 不整合)ことを実証する。
 *
 * 何が起きるか:
 *   - open(O_CREAT) 1回ごとに llfs_create が動き、
 *       inode bitmap(確保) + dir データブロック(dirent 追加) + inode テーブル
 *     の 3 ブロックを別々に dirty 化する(原子性も順序保証も無い)。
 *   - 各ファイルへブロック境界(4096B)ごとに書くと llfs_iomap_begin が
 *       block bitmap(確保) + inode テーブル(block[]/size)
 *     を更新する。
 *   - 1 操作ごとに sync() でディスク反映を強制 → クラッシュ窓を多数作る。
 *     データ書き込み(write-loop)より「途中状態」が長く露出するため捕まえやすい。
 *
 * 現状 llfs の制約(= write-loop のような無限ループにはできない理由):
 *   - unlink / truncate 未実装 → 作ったファイルは消せない。
 *   - inode は最大 64(0,1 は予約/ルート)→ 作成は約 62 ファイルで頭打ち。
 *   よって「create + block 割当」フェーズで可能な限り metadata を回し、
 *   使い切ったら idle して外部からの電源断を待つ。
 *   (恒久的な churn には unlink/truncate の実装が必要 → ジャーナリング後の課題)
 *
 * ビルド:  gcc -static -O2 meta-loop.c -o initramfs/root/meta-loop
 * 実行   :  start.sh から /root/meta-loop を起動(引数でマウント先を変更可)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* llfs.h の LLFS_N_BLOCKS と一致させること(直接ブロックのみ = 13) */
#define N_BLOCKS 13
#define BLOCK_SIZE 4096

/* start.sh で llfs をマウントする場所 */
#define DEFAULT_DIR "/mnt/host/llfs/mnt"

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : DEFAULT_DIR;
    unsigned long ops = 0;

    fprintf(stderr, "meta-loop: churning metadata under %s\n", dir);

    for (unsigned int n = 0;; n++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/f%05u", dir, n);

        int fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            /* inode を使い切った等。やり直す手段が無いので churn フェーズ終了。 */
            fprintf(stderr, "meta-loop: open(%s) failed: %s (created %u files, %lu ops)\n", path,
                    strerror(errno), n, ops);
            break;
        }

        /*
         * 各ブロック境界に「ファイル名:ブロック番号」を書く。
         *  - 1 回ごとに新しい block の割当(block bitmap + itable)を誘発
         *  - 内容はファイル名から一意に決まる → 後で fsck.py / 目視で整合を確認できる
         */
        for (int blk = 0; blk < N_BLOCKS; blk++) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "f%05u:%02d\n", n, blk);

            if (lseek(fd, (off_t)blk * BLOCK_SIZE, SEEK_SET) < 0)
                break;
            if (write(fd, buf, len) != len)
                break;
            sync(); /* ← ここで電源断が入ると metadata が中途半端に残る */
            ops++;
        }
        close(fd);
    }

    /*
     * これ以上 metadata を増やせない。プロセスは生かしたまま待機し、
     * 外部からの電源断(QEMU kill / panic)を受ける。
     * ここでの再書き込みは既存ブロックの上書きで metadata を更新しないため行わない。
     */
    fprintf(stderr, "meta-loop: churn done (%lu ops). idling, waiting for power cut...\n", ops);
    for (;;)
        pause();

    return 0;
}
