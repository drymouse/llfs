#!/usr/bin/env python3
"""
fsck.py - llfs のオフライン整合チェッカ(簡易 fsck)。

vdisk.img を *生で* 読み、メタデータ三者
  (1) inode ビットマップ
  (2) ブロックビットマップ
  (3) inode テーブル + ルートのディレクトリエントリ
が互いに矛盾していないかを検査する。readdir がスタブでも raw 読みなので影響を受けない。

用途:
  - クリーンに unmount したイメージ            → "CONSISTENT" になるべき
  - meta-loop 実行中に電源断したイメージ        → 不整合が報告されるはず(= 現状 llfs の問題実証)
  - ジャーナリング実装 + リカバリ後のイメージ   → 再び "CONSISTENT" になるべき(= 目標)

使い方:  python fsck.py <image>            (既定では vdisk.img を想定)
レイアウトは mkfs.py と一致(ジャーナル無しの現行版)。
"""
import sys
import struct

# --- レイアウト定数(mkfs.py / llfs.h と一致させること) ---
BLOCK_SIZE = 4096
INODE_SIZE = 64
N_BLOCKS = 13
SB_BLOCK = 1
ROOT_INO = 1
MAX_INODES = BLOCK_SIZE // INODE_SIZE  # 64

DIRENT_HEADER = 8  # inode(4) + rec_len(2) + name_len(1) + file_type(1)

# mkfs が常に使用済みにする固定領域(予約/SB/itable/2 bitmap/journal/root-data)。
# [JOURNAL] block 0..(journal_block + journal_blocks) を常に「使用済み」とみなす。
# 具体値は parse_super が読んだ journal フィールドから算出する(下の main 参照)。


def rd_block(img, n):
    return img[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE]


def bit_set(block_bytes, i):
    """LSB 起点(カーネル set_bit / mkfs と同じ並び)で i ビット目が立っているか。"""
    return (block_bytes[i // 8] >> (i % 8)) & 1


def parse_super(img):
    b = rd_block(img, SB_BLOCK)
    magic, block_size, itable, ibmap, bbmap = struct.unpack_from("<IIIII", b, 0)
    (inode_size,) = struct.unpack_from("<H", b, 20)
    # [JOURNAL] offset 22: pad(2) / journal_block(4) / journal_blocks(4)
    journal_block, journal_blocks = struct.unpack_from("<II", b, 24)
    return {
        "magic": magic,
        "block_size": block_size,
        "itable_block": itable,
        "inode_bitmap_block": ibmap,
        "block_bitmap_block": bbmap,
        "inode_size": inode_size,
        "journal_block": journal_block,
        "journal_blocks": journal_blocks,
    }


def parse_inode(itable_bytes, ino):
    off = ino * INODE_SIZE
    mode, uid, size, blocks = struct.unpack_from("<HHII", itable_bytes, off)
    block = list(struct.unpack_from("<%dI" % N_BLOCKS, itable_bytes, off + 12))
    return {"mode": mode, "uid": uid, "size": size, "blocks": blocks, "block": block}


def parse_dirents(block_bytes):
    """1 ディレクトリブロックを走査して (ino, name, rec_len, name_len) を列挙。"""
    out = []
    p = 0
    while p + DIRENT_HEADER <= BLOCK_SIZE:
        ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", block_bytes, p)
        if rec_len < DIRENT_HEADER:
            break  # 壊れている / 終端
        name = block_bytes[p + DIRENT_HEADER:p + DIRENT_HEADER + name_len]
        out.append((ino, name.decode("latin1"), rec_len, name_len, ftype))
        p += rec_len
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "vdisk.img"
    with open(path, "rb") as f:
        img = f.read()

    sb = parse_super(img)
    itable = rd_block(img, sb["itable_block"])
    ibmap = rd_block(img, sb["inode_bitmap_block"])
    bbmap = rd_block(img, sb["block_bitmap_block"])

    # [JOURNAL] 固定領域 = block 0..(journal_block + journal_blocks)。
    # journal_blocks==0(旧レイアウト)なら従来どおり block 0..5 を固定とみなす。
    if sb["journal_blocks"]:
        fixed_used_max = sb["journal_block"] + sb["journal_blocks"]  # = root-data block
    else:
        fixed_used_max = 5

    problems = []

    # --- (A) inode テーブルとルート dirent を読む ---
    inodes = {ino: parse_inode(itable, ino) for ino in range(MAX_INODES)}

    # ルート inode のデータブロックから全 dirent を収集
    root = inodes[ROOT_INO]
    dirent_targets = {}   # ino -> [names]  (このディレクトリから参照されている inode)
    for b in root["block"]:
        if b == 0:
            continue
        for ino, name, rec_len, name_len, ftype in parse_dirents(rd_block(img, b)):
            if ino == 0:
                continue
            if name in (".", ".."):
                continue
            dirent_targets.setdefault(ino, []).append(name)

    # --- (B) inode bitmap が指す使用中 inode ---
    ibmap_used = {ino for ino in range(MAX_INODES) if bit_set(ibmap, ino)}

    # --- (C) inode 三者整合 (bitmap / dirent / itable.mode) ---
    # 予約 inode 0、ルート 1 は常に live とみなす
    for ino in range(2, MAX_INODES):
        in_bitmap = ino in ibmap_used
        referenced = ino in dirent_targets
        has_mode = inodes[ino]["mode"] != 0

        if not (in_bitmap or referenced or has_mode):
            continue  # 完全に未使用 = 正常

        if in_bitmap and referenced and has_mode:
            continue  # 完全に生きている = 正常

        # ここに来たら三者のどれかが食い違っている
        names = dirent_targets.get(ino, [])
        detail = (
            f"inode {ino}: bitmap={'used' if in_bitmap else 'free'}, "
            f"dirent={names if names else 'none'}, "
            f"itable.mode={'set' if has_mode else 'ZERO'}"
        )
        if referenced and not has_mode:
            problems.append("[DANGLING] dirent が未初期化 inode を指す: " + detail)
        elif referenced and not in_bitmap:
            problems.append("[DANGLING] dirent が bitmap 上 free の inode を指す: " + detail)
        elif in_bitmap and not referenced:
            problems.append("[LEAK] bitmap で確保済みだが dirent から参照無し(リーク inode): " + detail)
        elif has_mode and not referenced:
            problems.append("[LEAK] itable に書かれたが dirent 無し(孤立 inode): " + detail)
        else:
            problems.append("[INCONSISTENT] inode 三者不一致: " + detail)

    # --- (D) ブロック割当の整合 (block bitmap vs inode.block[]) ---
    block_owner = {}  # block -> [inode...]
    for ino in range(MAX_INODES):
        for b in inodes[ino]["block"]:
            if b == 0:
                continue
            block_owner.setdefault(b, []).append(ino)

    # クロスリンク: 同じブロックを複数 inode が参照
    for b, owners in block_owner.items():
        if len(owners) > 1:
            problems.append(f"[CROSSLINK] block {b} を複数 inode が参照: {owners}")

    # inode が参照しているのに bitmap が free → 二重割当の危険
    for b, owners in block_owner.items():
        if b < BLOCK_SIZE * 8 and not bit_set(bbmap, b):
            problems.append(
                f"[UNMARKED] inode {owners} が参照する block {b} が bitmap 上 free"
            )

    # bitmap が used なのに誰も参照していない(固定領域を除く) → リークブロック
    for b in range(fixed_used_max + 1, BLOCK_SIZE * 8):
        if bit_set(bbmap, b) and b not in block_owner:
            problems.append(f"[LEAK] block {b} は bitmap used だが参照する inode 無し(リークブロック)")

    # --- 結果 ---
    print(f"image           : {path}")
    print(f"magic           : 0x{sb['magic']:08x}")
    print(f"inodes used (bm): {sorted(ibmap_used)}")
    print(f"dirents (root)  : "
          + ", ".join(f"{n}->ino{ino}" for ino, ns in sorted(dirent_targets.items()) for n in ns))
    print(f"blocks used (bm): {sum(1 for b in range(BLOCK_SIZE*8) if bit_set(bbmap, b))}")
    print("-" * 60)
    if not problems:
        print("CONSISTENT: メタデータの不整合は検出されませんでした。")
        sys.exit(0)
    else:
        print(f"INCONSISTENT: {len(problems)} 件の不整合を検出:")
        for p in problems:
            print("  - " + p)
        sys.exit(1)


if __name__ == "__main__":
    main()
