# LLFS 現状仕様まとめ

> このドキュメントは LLFS の現状仕様の単一情報源(source of truth)です。
> 実装やユーザによる加筆で随時更新されます。

## 1. 概要

- **名称**: `llfs`(LLUUIIGGEE FS)
- **種別**: **ブロックデバイスベース**のファイルシステム(`get_tree_bdev` を使用)
- **ライセンス**: GPL
- **ビルド対象カーネル**: `../../linux-7.0.10`(cachyos 7.0.10)
- **構成**: `super.o` + `inode.o` + `iomap.o` → `llfs.ko`

## 2. オンディスク・レイアウト(`llfs.h`)

### 設計上の前提
- **inode テーブル・各ビットマップは、それぞれ1ブロックに収まる**ものと仮定する。
- それらの配置ブロック番号は**スーパーブロックが保持**する
  (`itable_block` / `inode_bitmap_block` / `block_bitmap_block`)。
  → レイアウトは固定ではなく、スーパーブロックの値が正となる。

### 既定レイアウト(`mkfs.py` が作る実際の配置)

| ブロック番号 | 内容 | 参照元 |
|---|---|---|
| 0 | 予約(ブート用、未使用) | — |
| 1 | スーパーブロック (`struct llfs_super_block`) | `LLFS_SB_BLOCK` |
| 2 | inode テーブル(1ブロック, 最大64 inode) | `sb.itable_block` |
| 3 | inode ビットマップ(1ブロック) | `sb.inode_bitmap_block` |
| 4 | ブロックビットマップ(1ブロック) | `sb.block_bitmap_block` |
| 5 | ルートディレクトリのデータブロック | ルート inode `block[0]` |
| 6〜 | 空きデータブロック | — |

- **ブロックサイズ**: 4096 バイト(`sb_set_blocksize(sb, 4096)`、`sb.block_size` にも保持)
- **inode サイズ**: 64 バイト(`sb.inode_size`)
- **ルート inode 番号**: `LLFS_ROOT_INODE = 1`(inode 0 は予約/未使用)
- **inode 番号 → テーブル内オフセット**: `itable_block` 内の `ino * inode_size` バイト
  (inode 0 もスロットを占有するが未使用)
- **ビットマップのビット並び**: LSB 起点(カーネルの `set_bit`/`find_first_zero_bit` と同じ)。
  inode bitmap は inode 0,1 を、block bitmap は block 0〜5 を使用済みで初期化

### ディレクトリエントリ形式(ext2 風・可変長)

- 各データブロック内に `struct llfs_dir_entry` を連結。各エントリは `rec_len` バイト。
- **最後のエントリの `rec_len` はブロック末尾まで伸びる**(空き領域を吸収)。
- `inode == 0` は空きスロット。`name` は `name_len` バイト(NUL 終端なし)。
- ヘッダは 8 バイト(`inode` 4 + `rec_len` 2 + `name_len` 1 + `file_type` 1)。
- ルートには `.`(rec_len=12)と `..`(rec_len=ブロックサイズ−12)を作成。両方 `file_type=LLFS_FT_DIR`、`inode=1`。
- `file_type`: `LLFS_FT_UNKNOWN=0` / `LLFS_FT_REG=1` / `LLFS_FT_DIR=2`

### この設計から導かれる上限値

| 項目 | 計算 | 値 |
|---|---|---|
| 1ブロックあたりの inode 数 | 4096 / 64 | 64 |
| 最大 inode 数(テーブル1ブロック) | = 上記 | **64** |
| inode ビットマップ容量 | 4096 × 8 | 32768 ビット(余裕) |
| ブロックビットマップ容量 | 4096 × 8 | 32768 ビット |
| **FS 最大サイズ** | 32768 × 4096 | **128 MiB** |
| **最大ファイルサイズ** | 13 × 4096(直接のみ) | **52 KiB** |

### オンディスク構造体(`llfs.h` 現行)

```c
struct llfs_super_block {       // 64バイト境界に収める想定
    __le32 magic;               // マジックナンバー
    __le32 block_size;          // 1ブロックのサイズ(4096)
    __le32 itable_block;        // inode テーブルのブロック番号
    __le32 inode_bitmap_block;  // inode ビットマップのブロック番号
    __le32 block_bitmap_block;  // ブロックビットマップのブロック番号
    __le16 inode_size;          // inode サイズ(llfs では 64)
};

struct llfs_inode {             // 合計 64 バイト
    __le16 mode;                // ファイルモード
    __le16 uid;                 // 所有者 UID
    __le32 size;                // ファイルサイズ(バイト)
    __le32 blocks;              // 使用ブロック数
    __le32 block[LLFS_N_BLOCKS];// 直接ブロックへのポインタ(N=13, 間接なし)
};

struct llfs_dir_entry {         // 可変長
    __le32 inode;               // inode 番号
    __le16 rec_len;             // エントリ長
    __u8   name_len;            // ファイル名長(最大 LLFS_NAME_MAXLEN=255)
    __u8   file_type;           // ファイル種別
    char   name[];              // ファイル名
};

struct llfs_itable {            // inode テーブル(1ブロック内)
    struct llfs_inode table[];
};
```

### インメモリ構造体(`llfs.h` 現行)

```c
struct llfs_sb_info {       // sb->s_fs_info に格納(マクロ llfs_get_sb_info)
    u32 magic;              // ネイティブ型。fill_super で le→cpu 変換済み
    u32 block_size;
    u32 itable_block;
    u32 inode_bitmap_block;
    u32 block_bitmap_block;
    u16 inode_size;
};

struct llfs_inode_info {    // inode->i_private に格納(マクロ llfs_get_inode_info)
    __le32 blocks;          // ※まだ __le* のまま。配線時にネイティブ型へ要変換
    __le32 block[LLFS_N_BLOCKS];
};
```

> **配線済み**: `sb->s_fs_info`(= `llfs_sb_info`)は `llfs_fill_super` で設定し、
> `llfs_kill` で `kfree`。エンディアンはネイティブ型へ変換して保持。
>
> **未配線**: `inode->i_private`(= `llfs_inode_info`)はまだ未割り当て。
> こちらも配線時に `__le*` → ネイティブ型へ変換するのが望ましい。

## 3. マウント・フロー(`super.c`)

```
register_filesystem(&llfs)
  └─ init_fs_context = llfs_init
        └─ fc->ops = fc_ope { .get_tree = llfs_get_tree }
              └─ llfs_get_tree → get_tree_bdev(fc, llfs_fill_super)
                    └─ llfs_fill_super
```

### `llfs_fill_super` の処理
1. ブロックサイズを `LLFS_BLOCK_SIZE`(4096)に設定
2. **`LLFS_SB_BLOCK`(=1)** を `sb_bread` で読む
3. オンディスク `struct llfs_super_block` を `le32_to_cpu` / `le16_to_cpu` で
   **1度だけ変換**して `struct llfs_sb_info`(ネイティブ型)に取り込み、
   `sb->s_fs_info` にセット。`sb->s_magic = sbi->magic`
4. 取り込み後ただちに `brelse(bh)`(以降ディスクバッファは不要)
5. `block_size != 4096` なら `-EINVAL`
6. ルート inode を生成(`new_inode`)
   - `i_ino = LLFS_ROOT_INODE`(=1)
   - `i_mode = S_IFDIR | 0755`
   - `i_mapping->a_ops = llfs_asops`
   - `i_op = llfs_dir_iop`、`i_fop = simple_dir_operations`
7. `d_make_root` でルート dentry 作成 → `sb->s_root`

> エラー時は `bh` / `sbi` を解放。`sbi` の最終解放は `llfs_kill`。

### アンマウント
- `kill_sb = llfs_kill`: `kfree(sb->s_fs_info)` → `kill_block_super(sb)`

## 4. inode / ファイル操作(`inode.c`)

| オペレーション | 内容 |
|---|---|
| `super_operations llfs_sop` | `.statfs = simple_statfs`, `.evict_inode = llfs_evict_inode`(**`sb->s_op` に割当済み**) |
| `inode_operations llfs_dir_iop` | `.create = llfs_create`, `.lookup = llfs_lookup` |
| `inode_operations llfs_file_iop` | **空 `{}`** |
| `file_operations llfs_file_fop` | `generic_file_read_iter` / `llfs_file_write_iter`(iomap) / `generic_file_mmap` / `generic_file_llseek` |

### `llfs_iget`(inode をディスクから読む)
- `iget_locked` で inode キャッシュを使い、同一 `ino` の重複生成を防ぐ
- `itable_block` 内の `ino * inode_size` から `struct llfs_inode` を読み、
  `le16/le32_to_cpu` で `i_mode` / `uid` / `i_size` と `llfs_inode_info`(`block[]`)を構築
- mode に応じて `i_op` / `i_fop` / nlink を設定、`a_ops = llfs_asops`
- `i_private` に `llfs_inode_info` を保持(解放は `llfs_evict_inode` で `kfree`)

### `llfs_lookup`
- `dir` の各データブロックを走査し、`name_len` 一致 + `memcmp` で名前一致を探す
- 見つかれば `llfs_iget(sb, ino)` → `d_splice_alias`
- 見つからなければ `d_splice_alias(NULL, dentry)` で負の dentry を作成

### `llfs_make_inode` / `llfs_create`(※一部永続化済み・なお要修正)
- `llfs_get_first_free_inode` で inode bitmap から空き inode を確保(`set_bit` + `mark_buffer_dirty`)
- `new_inode` で生成 → `inode_init_owner` → `d_instantiate`
- **itable への mode 書き込み**: `llfs_make_inode` が `llfs_get_itable` で itable を取得し、
  `itable->table[ino].mode = cpu_to_le16(mode)` を書いて `mark_buffer_dirty`
  (※ mode のみ。`size` / `blocks` / `block[]` はまだ未書き込み)
- **ディレクトリエントリへの追加**: `llfs_add_dirent` が dir の block[0] を読み、
  dirent チェーンを走査して新エントリを書き込み `mark_buffer_dirty`
  → **`llfs_get_block` の成否判定が反転していた不具合を修正し、dirent が更新されるようになった**

#### `llfs_add_dirent` の現状の制約(要修正の沼候補)
1. **空きスロット探索が ext2 流の「最後のエントリを分割」に未対応** —
   `inode == 0` のスロットを線形探索するだけ。`mkfs.py` のルートは `.`/`..` のみで
   `inode==0` の空きが無く、`..` の `rec_len` がブロック末尾まで伸びているため、
   走査がブロック末尾を越えてバッファ外参照になりうる(last-entry split が必要)
2. **エンディアン未変換** — 走査時の `ptr += dirent->rec_len` や書き込み時の
   `rec_len` / `inode` 設定で `le16_to_cpu` / `cpu_to_le16` 等を一部通していない
3. **`rec_len` の計算が不正確** — `name_len + 9` としているが、ヘッダは 8 バイトなので
   最小長は `8 + name_len`(+ 4バイト境界丸め)が正
4. **`file_type` が `LLFS_FT_REG` 固定** — `mode` から `LLFS_FT_DIR` 等へ出し分けるべき(TODO)
5. **`llfs_create` の inode エラー判定** — `llfs_make_inode` は `ERR_PTR` を返すのに
   `if (!inode)`(NULL チェック)で受けており、`IS_ERR(inode)` で見るべき

## 5. アドレス空間 / iomap(`iomap.c`)

| オペレーション | 内容 |
|---|---|
| `address_space_operations llfs_asops` | `read_folio` / `readahead` / `writepages`(+ 検討中の `write_begin`/`write_end`) |
| `iomap_ops llfs_iomap_ops` | `iomap_begin` / `iomap_end`(**両方スタブ、`return 0` のみ**) |
| `iomap_writeback_ops llfs_writeback_ops` | **空 `{}`** |

- `read_folio` → `iomap_bio_read_folio`
- `readahead` → `iomap_bio_readahead`
- `writepages` → `iomap_writepages`
- 書き込み経路は現在迷走中(`generic_file_write_iter` ↔ `iomap_file_buffered_write` の切替を検討)

### v7.0.10 の iomap API メモ
- `iomap_file_buffered_write(iocb, from, ops, write_ops, private)` の **5引数**
- `struct iomap_write_ops` が追加(`get_folio` / `put_folio` / `iomap_valid` / `read_folio_range`、いずれもオプション)
- `write_ops` 引数自体に NULL は不可。空構造体への非NULLポインタを渡す

## 6. デバッグ環境(`command.py`)

- GDB スクリプト。QEMU の `:12345` にリモート接続
- `loadsym` コマンドで `llfs.ko` を `0xffffffffc0000000` にロードし、`panic` / `read_pages` にブレークポイント

---

## 7. 「動く」状態と未実装の境界

### 動作確認済み(ビルドレベル)
- モジュールロード / FS 登録
- マウント → スーパーブロックを `llfs_sb_info` に取り込み(ネイティブ型)
- `llfs_iget` でルート inode を itable から読み込み
- `mkfs.py` が有効なイメージ(SB / root inode / `.`/`..` / 両 bitmap)を生成

### スタブ / 未実装
- `iomap_begin` / `iomap_end`(実マッピングなし → 読み書きの実体が動かない)
- **readdir**: ルートは `simple_dir_operations`(dcache ベース)のため、
  オンディスクのディレクトリエントリを `ls` で列挙しない(名前指定の lookup は動く)
- **create の永続化(部分的に実装)**:
  - 済: inode bitmap の確保、itable への mode 書き込み、ディレクトリエントリへの追加
  - 未: itable への `size` / `blocks` / `block[]` 書き込み、dir の `i_size` 更新、
    `llfs_add_dirent` の ext2 流 last-entry split / エンディアン / `rec_len` / `file_type` 修正
    (詳細はセクション4参照)
- ブロック割り当て(block bitmap を使った data block 確保)

### 矛盾・要注意点(沼の原因候補)
1. **create と lookup の非対称(緩和中)** — create は itable mode と dirent を書き戻すようになったが、
   `size` / `block[]` 等は未永続化。`llfs_add_dirent` のスロット探索も未完成のため、
   ルートに既存の `.`/`..` しか無い状態だと走査がバッファ外に出るリスクが残る
2. `llfs.h` で **`NULL` を `(unsigned long)0` に再定義** — ポインタ文脈で型的に危険
3. `llfs_fill_inode_info` は現状未使用(`__maybe_unused`)。iomap 経路で使う想定
   (`llfs_get_block` は `llfs_add_dirent` から使用されるようになった)

### 解決済み(過去の沼)
- ~~`llfs_add_dirent` が成否判定反転で即 return~~ → `if (!llfs_get_block(...))` を
  `if (llfs_get_block(...))` に修正。dirent が更新されるようになった
- ~~`sb->s_op` 未割当~~ → `llfs_sop` を割当(`evict_inode` で `i_private` 解放)
- ~~lookup が常に ino=2~~ → ディレクトリ走査 + `llfs_iget` + `d_splice_alias`
- ~~`kill_sb` が `kill_block_super` を呼ばない~~ → 呼ぶように修正(+ `s_fs_info` 解放)
- ~~インメモリ構造体が `__le*`~~ → `llfs_sb_info` / `llfs_inode_info` をネイティブ型へ
