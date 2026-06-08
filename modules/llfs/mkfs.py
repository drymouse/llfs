import sys
import struct

# --- レイアウト定数(カーネル側 llfs.h と一致させること) ---
MAGIC = 0x90909090
BLOCK_SIZE = 4096
INODE_SIZE = 64
N_BLOCKS = 13

SB_BLOCK = 1  # スーパーブロック
ITABLE_BLOCK = 2  # inode テーブル
IBITMAP_BLOCK = 3  # inode ビットマップ
BBITMAP_BLOCK = 4  # ブロックビットマップ
ROOT_DATA_BLOCK = 5  # ルートディレクトリのデータブロック
FIRST_FREE_BLOCK = 6  # 最初の空きブロック(これ以降がデータ領域)

ROOT_INO = 1  # inode 0 は予約。ルートは inode 1

S_IFDIR = 0o040000
ROOT_MODE = S_IFDIR | 0o755

# llfs_dir_entry.file_type
FT_DIR = 2

DIRENT_HEADER = 8  # inode(4) + rec_len(2) + name_len(1) + file_type(1)


def make_dirent(ino, name, ftype, rec_len):
    """ext2 風の可変長ディレクトリエントリを rec_len バイトに詰めて返す。"""
    nb = name.encode()
    body = struct.pack("<IHBB", ino, rec_len, len(nb), ftype) + nb
    assert len(body) <= rec_len
    return body + b"\x00" * (rec_len - len(body))


def write_super(f):
    # struct llfs_super_block: __le32 x5 + __le16(inode_size)
    f.seek(BLOCK_SIZE * SB_BLOCK)
    f.write(
        struct.pack(
            "<IIIIIH",
            MAGIC,
            BLOCK_SIZE,
            ITABLE_BLOCK,
            IBITMAP_BLOCK,
            BBITMAP_BLOCK,
            INODE_SIZE,
        )
    )


def write_inode(f, ino, mode, uid, size, blocks, block_ptrs):
    # struct llfs_inode (64B): mode(u16) uid(u16) size(u32) blocks(u32) block[13](u32)
    arr = list(block_ptrs) + [0] * (N_BLOCKS - len(block_ptrs))
    raw = struct.pack("<HHII", mode, uid, size, blocks) + struct.pack(
        "<%dI" % N_BLOCKS, *arr
    )
    assert len(raw) == INODE_SIZE
    f.seek(BLOCK_SIZE * ITABLE_BLOCK + ino * INODE_SIZE)
    f.write(raw)


def write_root_dir(f):
    # "." と ".." を書き、".." の rec_len でブロック末尾まで埋める
    dot = make_dirent(ROOT_INO, ".", FT_DIR, DIRENT_HEADER + 4)  # 12B
    dotdot = make_dirent(ROOT_INO, "..", FT_DIR, DIRENT_HEADER + 4)
    f.seek(BLOCK_SIZE * ROOT_DATA_BLOCK)
    f.write(dot + dotdot)


def set_bits(f, block, count):
    """block の先頭から count ビットを 1 にする(LSB から。カーネルの set_bit と同じ並び)。"""
    nbytes = (count + 7) // 8
    data = bytearray(nbytes)
    for i in range(count):
        data[i // 8] |= 1 << (i % 8)
    f.seek(BLOCK_SIZE * block)
    f.write(bytes(data))


def main():
    if len(sys.argv) < 2:
        print("usage: python mkfs.py <image>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb+") as f:
        size = f.seek(0, 2)
        f.seek(0)
        f.write(b"\0" * size)

        write_super(f)
        # ルート inode(= inode 1)。データブロックは ROOT_DATA_BLOCK 1 個。
        write_inode(f, ROOT_INO, ROOT_MODE, 0, BLOCK_SIZE, 1, [ROOT_DATA_BLOCK])
        write_root_dir(f)
        # inode ビットマップ: inode 0(予約)と 1(ルート)を使用済みに
        set_bits(f, IBITMAP_BLOCK, 2)
        # ブロックビットマップ: block 0..ROOT_DATA_BLOCK を使用済みに
        set_bits(f, BBITMAP_BLOCK, ROOT_DATA_BLOCK + 1)
        # 少なくとも最初の空きブロックの手前までイメージを確保
        f.seek(0, 2)
        if f.tell() < BLOCK_SIZE * FIRST_FREE_BLOCK:
            f.truncate(BLOCK_SIZE * FIRST_FREE_BLOCK)


if __name__ == "__main__":
    main()
