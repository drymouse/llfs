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
| `super_operations llfs_sop` | `.statfs = simple_statfs`, `.evict_inode = llfs_evict_inode`, `.write_inode = llfs_write_inode`(**`sb->s_op` に割当済み**) |
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

### `llfs_write_inode`(inode メタデータの唯一の永続化窓口)
- `super_operations.write_inode` として登録。`mark_inode_dirty` 起点で VFS が呼ぶ
- `llfs_get_itable` で itable を読み、`itable->table[i_ino]` へ
  `mode` / `uid` / `size`(= `i_size`)/ `blocks`(= `i_blocks`)を `cpu_to_le*` で直列化し、
  `block[]` は `memcpy(dinode->block, inodei->block, ...)` で丸ごとコピー → `mark_buffer_dirty`
- `inode_info` は **`llfs_get_inode_info_checked`** で取得(NULL ガード)。`touch` 直後など
  一度も `iomap_begin` を通っていない inode(`i_private==NULL`)でも itable から遅延充填され、
  NULL 参照を起こさない
- **方針**: 「書き換えは各所(`iomap_begin` 等)、ディスク反映は `write_inode` 一箇所」。
  これにより `llfs_make_inode` が以前やっていた itable 直書きは廃止(コードはコメントアウト済み)

### `llfs_make_inode` / `llfs_create`(mount/touch で動作確認済み)
- `llfs_get_first_free_inode` で inode bitmap から空き inode を確保(`set_bit` + `mark_buffer_dirty`)
- `new_inode` で生成 → `i_ino` 設定 → **`insert_inode_hash(inode)`** → `i_op`/`i_fop`/nlink 設定
  → `i_blocks=0` / `i_size=0` / `a_ops`
  (itable への書き込みはここでは行わない。永続化は後続の `mark_inode_dirty`→`write_inode`)
- **`insert_inode_hash` が必須**: `new_inode()` は hash 登録をしないため、未登録のままだと
  `__mark_inode_dirty` が `inode_unhashed()` で弾いて superblock の dirty list に載せず、
  `write_inode` が一切呼ばれない(`fs/fs-writeback.c` の「Only add valid (hashed) inodes」分岐)。
  root(ino=1)は `iget_locked` 経由で hash 済みなので影響を受けず、新規ファイルだけ永続化されなかった
- `llfs_create`: `llfs_make_inode` → `inode_init_owner` → `llfs_add_dirent` → `d_instantiate`
  → `mark_inode_dirty(dir)` / `mark_inode_dirty(inode)`
- **ディレクトリエントリへの追加**: `llfs_add_dirent` が dir の block[0] を読み、
  dirent チェーンを走査して新エントリを書き込み `mark_buffer_dirty`

#### `llfs_add_dirent` の現状の制約(要修正の沼候補)
1. **空きスロット探索が ext2 流の「最後のエントリを分割」に未対応** —
   `inode == 0` のスロットを線形探索するだけ。`mkfs.py` のルートは `.`/`..` のみで
   `inode==0` の空きが無く、`..` の `rec_len` がブロック末尾まで伸びているため、
   走査がブロック末尾を越えてバッファ外参照になりうる(last-entry split が必要)
2. **`rec_len` の計算**: `((name_len >> 2) << 2) + 4 + 8`(= 4バイト境界丸め + ヘッダ8)
   としエンディアン変換済み。走査側 `ptr += le16_to_cpu(dirent->rec_len)` も変換済み
3. **`file_type` が `LLFS_FT_REG` 固定** — `mode` から `LLFS_FT_DIR` 等へ出し分けるべき(TODO)
4. **`llfs_create` の inode エラー判定** — `llfs_make_inode` は `ERR_PTR` を返すのに
   `if (!inode)`(NULL チェック)で受けており、`IS_ERR(inode)` で見るべき(`llfs_add_dirent`
   の戻り値も無視している)

## 5. アドレス空間 / iomap(`iomap.c`)

| オペレーション | 内容 |
|---|---|
| `address_space_operations llfs_asops` | `read_folio` / `readahead` / `writepages` |
| `iomap_ops llfs_iomap_ops` | `iomap_begin`(実マッピング)/ `iomap_end`(サイズ変更を dirty 化) |
| `iomap_writeback_ops llfs_writeback_ops` | `writeback_range = llfs_writeback_range`, `writeback_submit = iomap_ioend_writeback_submit` |
| `iomap_write_ops llfs_iomap_wops` | **空 `{}`**(`iomap_file_buffered_write` の第4引数に非NULLで渡す用) |

- read: `read_folio` → `iomap_bio_read_folio` / `readahead` → `iomap_bio_readahead`
- buffered write: `llfs_file_write_iter` → `iomap_file_buffered_write(.., &llfs_iomap_ops, &llfs_iomap_wops, NULL)` → `generic_write_sync`
- writeback: `writepages` → `iomap_writepages` → `llfs_writeback_range`

### `llfs_iomap_begin`(論理→物理ブロック変換)
- `sector = offset >> LLFS_BLOCK_SIZE_SHIFT`。`sector >= LLFS_N_BLOCKS` は `-EIO`
- `llfs_get_inode_info_checked` で `inode_info` を遅延充填してから `block[sector]` を引く
- `block[sector] != 0`: `IOMAP_MAPPED`、`addr = bno << LLFS_BLOCK_SIZE_SHIFT`
- `block[sector] == 0` かつ read: `IOMAP_HOLE` / `addr = IOMAP_NULL_ADDR`(ゼロ埋め)
- `block[sector] == 0` かつ `IOMAP_WRITE`: `llfs_alloc_block` で block bitmap から確保し、
  `inode_info->block[sector] = bno` / `i_blocks++` / `mark_inode_dirty`(→ `write_inode` で永続化)

### `llfs_alloc_block`(block bitmap アロケータ)
- `sbi->block_bitmap_block` を読み `find_first_zero_bit` → `set_bit` → `mark_buffer_dirty`
- `inode bitmap` 版(`llfs_get_first_free_inode`)と同型。ロックは無し(単一スレッド前提)

### `llfs_iomap_end` / `llfs_writeback_range`
- `iomap_end`: `iomap->flags & IOMAP_F_SIZE_CHANGED` なら `mark_inode_dirty`(`i_size` 永続化を起動)
  ※ `i_size` 自体はカーネルの buffered write が更新済み(FSは反映のみ担当)
- `writeback_range`: キャッシュ済み `wpc->iomap` が `offset` を外れていたら
  `llfs_iomap_begin(.., IOMAP_WRITE, ..)` で張り直し → `iomap_add_to_ioend`(gfs2/zonefs と同型)

### v7.0.10 の iomap API メモ
- `iomap_file_buffered_write(iocb, from, ops, write_ops, private)` の **5引数**
- `struct iomap_write_ops` が追加(`get_folio` / `put_folio` / `iomap_valid` / `read_folio_range`、いずれもオプション)
- `write_ops` 引数自体に NULL は不可。空構造体への非NULLポインタを渡す

## 6. デバッグ環境(`command.py`)

- GDB スクリプト。QEMU の `:12345` にリモート接続
- `loadsym` コマンドで `llfs.ko` を `0xffffffffc0000000` にロードし、`panic` / `read_pages` にブレークポイント

---

## 7. 「動く」状態と未実装の境界

### 動作確認済み(実機・コマンドレベル)
- モジュールロード / FS 登録 / マウント(`llfs_sb_info` 取り込み、ネイティブ型)
- `llfs_iget` でルート inode を itable から読み込み
- `mkfs.py` が有効なイメージ(SB / root inode / `.`/`..` / 両 bitmap)を生成
- **`touch`**: 空ファイル作成(inode 確保 + dirent 追加 + `write_inode` 永続化)
- **`echo > file`**: buffered write → `iomap_begin` でブロック割当 → ページキャッシュ書込
- **`sync`**: `writepages`→`writeback_range` でデータを、`write_inode` でメタを itable へ書き戻し

### スタブ / 未実装
- **readdir**: ルートは `simple_dir_operations`(dcache ベース)のため、
  オンディスクのディレクトリエントリを `ls` で列挙しない(名前指定の lookup は動く)
- **`llfs_add_dirent` の ext2 流 last-entry split**(セクション4参照)。`file_type` も REG 固定
- 間接ブロック無し → 最大ファイルサイズ 52KiB(直接13ブロック)。`unlink` / `mkdir` / `truncate` 未実装
- `llfs_iomap_wops` / `llfs_file_iop` は空 `{}`

### 矛盾・要注意点(沼の原因候補)
1. **`blocks` カウントの意味が不統一** — `i_blocks` は本来 512B 単位だが
   `iomap_begin` は fs ブロックあたり +1 で増やし、`write_inode` はそれを `dinode->blocks` に直書き。
   一方 `iget` は `raw->blocks` を `inode_info->blocks` に読むだけで `i_blocks` には載せない
   → 再マウント後に blocks がラウンドトリップしない
2. **`bno << SHIFT` が32bit演算**(`iomap_begin`)。本FS最大では溢れないが `(u64)bno` キャスト推奨
3. `llfs.h` で **`NULL` を `(unsigned long)0` に再定義** — ポインタ文脈で型的に危険
4. `llfs_create` がエラーを握り潰す(`if (!inode)` で `ERR_PTR` を素通し、`llfs_add_dirent`
   の戻り値も無視)

### 解決済み(過去の沼)
- ~~新規ファイル(ino≥2)の `write_inode` が呼ばれず永続化されない~~ →
  `llfs_make_inode` に **`insert_inode_hash(inode)`** を追加。`new_inode()` は hash 登録しないため、
  未登録だと `__mark_inode_dirty` が dirty list に載せず writeback 対象外になっていた
  (root=ino1 は `iget_locked` で hash 済みのため唯一書き戻されていた)。
  併せて `write_inode` の `inode_info` 取得を **`llfs_get_inode_info_checked`** にして、
  hash 修正で到達するようになった `touch` 済み(`i_private==NULL`)inode の NULL 参照を予防。
  なお当初 `IOMAP_F_NEW` 修正が原因と疑ったが無関係(同フラグの効果は write_begin の
  ゼロ埋め判定 `iomap_block_needs_zeroing` のみで、`write_inode` の永続化経路には不関与)
- ~~`IOMAP_F_NEW` が実効していない~~ → `iomap->flags |= IOMAP_F_NEW`(正しいフィールド)に修正。
  新規割当ブロックがゼロ初期化され、部分書き込みでのゴミ露出が解消
- ~~iomap_begin/end・writeback_ops がスタブ~~ → 実マッピング実装。touch/echo/sync が動作
- ~~create の永続化が mode のみ~~ → `write_inode`(`.write_inode`)を導入し
  `size`/`blocks`/`block[]` も itable へ書き戻し。itable 直書きは `write_inode` に集約
- ~~`llfs_add_dirent` が成否判定反転で即 return~~ → `if (!llfs_get_block(...))` を
  `if (llfs_get_block(...))` に修正。dirent が更新されるようになった
- ~~`sb->s_op` 未割当~~ → `llfs_sop` を割当(`evict_inode` で `i_private` 解放)
- ~~lookup が常に ino=2~~ → ディレクトリ走査 + `llfs_iget` + `d_splice_alias`
- ~~`kill_sb` が `kill_block_super` を呼ばない~~ → 呼ぶように修正(+ `s_fs_info` 解放)
- ~~インメモリ構造体が `__le*`~~ → `llfs_sb_info` / `llfs_inode_info` をネイティブ型へ

---

## 8. ジャーナリング設計(WAL / 物理 redo ログ)

> このセクションは**設計(これから実装する)**。現状は未実装。
> 目的は、`write-loop` + `sync` + 突然の電源断(`boot.sh` の `panic=-1`/QEMU 強制終了)
> に対して、**再マウント後にファイルシステムのメタデータが常に一貫している**ことを保証すること。

### 8.1 解決したい問題(なぜ必要か)

現状、1つの高レベル操作(例: `touch` = `llfs_create`)は**複数の独立したブロック書き込み**に
分解され、それぞれが別個に `mark_buffer_dirty` → 任意のタイミングで writeback される:

| 操作 | 触るメタデータブロック |
|---|---|
| `create` | inode bitmap(確保)+ itable(inode 直列化)+ dir データブロック(dirent 追加) |
| 書き込み中のブロック割当(`iomap_begin`) | block bitmap(確保)+ itable(`block[]`/`blocks`/`size`) |
| `write_inode` | itable |

これらは**順序保証も原子性もない**。電源断が途中に入ると、例えば:
- block bitmap では「使用中」なのに itable の `block[]` に未反映 → **リークブロック**
- itable は block を指すのに bitmap が「空き」 → 同じブロックが**二重割当(クロスリンク)** → 破損
- dirent は inode を指すのに itable の当該 inode が未書き込み → **ダングリング参照**

ジャーナリングは、これら複数ブロック更新を**1つの原子的トランザクション(all-or-nothing)**にする。

### 8.2 採用方式と方針

- **物理ブロック単位の redo(やり直し)ログ** = ext3/jbd2 の "journaled" モードの簡略版。
  - 「論理(操作)ログ」ではなく「変更後ブロックの丸ごとコピー」を記録する。**冪等**で実装が単純。
- **メタデータのみジャーナルする**(data=writeback 相当)。ファイルデータはページキャッシュから
  最終ブロックへ直接 writeback(現状の `writepages` 経路のまま)。
  - 新規割当ブロックは `IOMAP_F_NEW` でゼロ初期化済みのため、データ未達で電源断しても
    **露出するのはゼロであってゴミではない**。メタデータの一貫性が保たれれば FS は壊れない。
  - 「最近書いたデータがロストし得る」点は許容(`fsync` のデータ順序保証は将来拡張、§8.9)。
- **単一トランザクション・ジャーナル**(同時に有効なトランザクションは常に1つ)。
  LLFS は単一スレッド前提なので循環ログ(wraparound)は不要。コミット完了→チェックポイント→
  ジャーナルクリア、を毎回行い、ジャーナルは「空 or 1件」の状態しか取らない。

#### なぜこの方式が LLFS に合うか
LLFS のメタデータ更新は**すべて `sb_bread` で得た buffer_head 経由**であり、FS 自身が全変更点を
握っている。よって「`mark_buffer_dirty` を即時に呼ばず、commit まで in-place 書き込みを遅延する」
という規律を**自前で**徹底でき、jbd2 のような複雑なバッファ状態機械なしに WAL 不変条件を守れる(§8.6)。

### 8.3 オンディスク・レイアウトの変更

スーパーブロックに journal 領域の位置・長さを追加(レイアウトはスーパーブロックが正、の原則を踏襲):

```c
struct llfs_super_block {       // 現行 22B → +8B = 30B(64B 境界内)
    __le32 magic;
    __le32 block_size;
    __le32 itable_block;
    __le32 inode_bitmap_block;
    __le32 block_bitmap_block;
    __le16 inode_size;
    __le16 pad;                 // 4B 境界調整(新規)
    __le32 journal_block;       // ジャーナル領域の先頭ブロック番号(新規)
    __le32 journal_blocks;      // ジャーナル領域のブロック数(新規)
};
```

`struct llfs_sb_info` にも `u32 journal_block; u32 journal_blocks;` を追加し、`fill_super` で変換取り込み。

**新レイアウト(`mkfs.py` を更新)**:

| ブロック番号 | 内容 |
|---|---|
| 0 | 予約 |
| 1 | スーパーブロック |
| 2 | inode テーブル |
| 3 | inode ビットマップ |
| 4 | ブロックビットマップ |
| **5 〜 5+J−1** | **ジャーナル領域(J ブロック)** ← 新規 |
| 5+J | ルートディレクトリのデータブロック |
| 6+J〜 | 空きデータブロック |

- ジャーナル長 `J` は当面 **32 ブロック(128 KiB)**。1トランザクションが触るメタは最大でも
  数ブロック(create で 3)なので十分。
- `mkfs.py` は **block bitmap でブロック 0〜(5+J) を使用済み**にマーク(ジャーナル領域を
  アロケータに渡さないため)。`journal_block=5` / `journal_blocks=J` を SB に書く。
  ジャーナル領域は**ゼロ初期化**(descriptor magic=0 = 空)。

### 8.4 ジャーナルのオンディスク形式

ジャーナル領域内は **[descriptor] [data 0] … [data n−1] [commit]** の連続レイアウト
(先頭 = `journal_block`)。

```c
#define LLFS_JRNL_DESC_MAGIC   0x4c4c4a44u  // "LLJD" descriptor
#define LLFS_JRNL_COMMIT_MAGIC 0x4c4c4a43u  // "LLJC" commit

/* descriptor ブロック(journal_block に置く): このトランザクションの内容目録 */
struct llfs_jrnl_desc {
    __le32 magic;        // LLFS_JRNL_DESC_MAGIC(0 ならジャーナルは空)
    __le32 sequence;     // トランザクション連番(commit と一致して初めて有効)
    __le32 n_blocks;     // 後続する data ブロック数
    __le32 target[];     // 各 data ブロックの最終(in-place)ブロック番号、n_blocks 個
};

/* commit ブロック(data の直後 = journal_block + 1 + n_blocks に置く) */
struct llfs_jrnl_commit {
    __le32 magic;        // LLFS_JRNL_COMMIT_MAGIC
    __le32 sequence;     // descriptor.sequence と一致 = コミット成立の証拠
};
```

- **コミット成立の判定**: descriptor.magic 有効 **かつ** commit ブロックの magic 有効 **かつ**
  両者の `sequence` 一致。トーン(torn write)した commit はマジック/連番不一致で弾かれる。
- descriptor は 1 ブロックで `target[]` を最大 (4096−12)/4 ≈ 1020 個保持でき、`J=32` の
  data 上限(30)に対し十分。

### 8.5 トランザクションのコミット手順(WAL プロトコル)

メタ更新は**まずジャーナルに書いて永続化してから**、最終位置に書く。バリア(デバイスフラッシュ)の
位置が肝。インメモリのトランザクション:

```c
#define LLFS_TXN_MAX 30
struct llfs_txn {
    struct super_block *sb;
    unsigned int n;                          // ステージ済みブロック数
    unsigned int target[LLFS_TXN_MAX];       // 最終ブロック番号
    struct buffer_head *bh[LLFS_TXN_MAX];    // in-place バッファ(変更済み・未 dirty・pin 中)
};

// 変更したメタ buffer_head を登録(mark_buffer_dirty の代わりに呼ぶ)
void llfs_txn_log(struct llfs_txn *t, struct buffer_head *bh);
```

`llfs_txn_commit(t)` の手順(`blkdev_issue_flush` or REQ_PREFLUSH/FUA でバリア):

```
1. descriptor を組み立て(magic, ++sequence, n, target[0..n-1])、journal_block へ書く
2. data 0..n-1 を journal_block+1.. へ書く(bh[i]->b_data を丸ごと)
3. ── BARRIER ──  ここまで(descriptor+data)をディスクに確実に永続化(flush)
4. commit ブロック(magic, 同 sequence)を journal_block+1+n へ書き、flush  ← ★コミット点
5. チェックポイント: 各 bh[i] を mark_buffer_dirty + sync_dirty_buffer で最終位置へ反映、flush
6. ジャーナルクリア: descriptor.magic=0 を書いて flush(次回マウントで replay されないように)
7. 各 bh[i] を brelse(pin 解除)、txn を解放
```

クラッシュ時の帰結:
- **手順4より前**で停止 → commit 無効 → リカバリは無視。in-place はまだ手付かず(=旧状態で一貫)。
- **手順4成立後〜手順6前**で停止 → リカバリが replay → 全 in-place を再現(=新状態で一貫)。
- **手順6後**で停止 → ジャーナル空 → 何もしない。
→ いずれも **all-or-nothing**。

### 8.6 バッファキャッシュ制御の方針(最重要)

WAL の不変条件は「**ジャーナル commit が永続化するまで、その操作のメタを最終位置へ書いてはならない**」。
現状コードは更新後すぐ `mark_buffer_dirty` するため、カーネルの定期 writeback が commit 前に
in-place へ書き出し得る。これを防ぐ規律:

- メタデータ更新箇所では **`mark_buffer_dirty` を即時に呼ばない**。代わりに、
  1. `sb_bread` で bh 取得 → b_data を変更
  2. `get_bh(bh)` で pin したまま `llfs_txn_log(t, bh)` に登録(`brelse` しない)
- in-place への実書き込みは**チェックポイント(手順5)で初めて** `mark_buffer_dirty` +
  `sync_dirty_buffer` する。それまで dirty にしないのでカーネルが勝手に書き出さない。
- 同一ブロック(例: itable)が同一トランザクションで複数回更新される場合は、bh は同一なので
  **重複登録を避ける**(`target` 重複チェック、または既登録 bh はスキップ)。

これにより、ext3/jbd2 のような専用バッファ状態管理を持ち込まずに不変条件を満たせる
(LLFS が全メタ更新点を握っているからこそ可能)。

### 8.7 リカバリ(マウント時 replay)

`llfs_fill_super` で **スーパーブロック取り込み直後・root inode を読む前**に
`llfs_journal_recover(sb)` を実行(itable/bitmap を信用する前に整合させる):

```
llfs_journal_recover(sb):
  desc = read(journal_block)
  if desc.magic != LLFS_JRNL_DESC_MAGIC: return        // ジャーナル空 → 何もしない
  n = desc.n_blocks
  commit = read(journal_block + 1 + n)
  if commit.magic != LLFS_JRNL_COMMIT_MAGIC
     || commit.sequence != desc.sequence:
       // 未コミット(torn) → 破棄。desc.magic=0 を書いて flush
       return
  // コミット済み → redo
  for i in 0..n-1:
      copy journal_block+1+i のデータを desc.target[i] へ(sb_bread→memcpy→mark_buffer_dirty→sync)
  flush
  desc.magic = 0; write+flush                            // クリア(冪等だが二度手間を避ける)
```

redo は**同一内容の上書き**なので冪等。replay 中に再度クラッシュしても、次回また replay すれば
同じ結果に収束する。

### 8.8 既存コードへの統合ポイント

| 場所 | 現状 | 変更 |
|---|---|---|
| `llfs.h` | SB 構造体 | `pad`/`journal_block`/`journal_blocks` 追加、`llfs_jrnl_*` 構造体、`llfs_txn` API、magic 定義 |
| `mkfs.py` | レイアウト | journal 領域を挿入、bbitmap に journal 範囲を使用済み計上、SB に journal フィールド書込 |
| `super.c` `fill_super` | SB 取込 | journal フィールド取込 → **`llfs_journal_recover` 呼出**(root 読込の前) |
| `inode.c` `llfs_get_first_free_inode` | `set_bit`+`mark_buffer_dirty`+`brelse` | bh を pin して `llfs_txn_log`(dirty/brelse しない) |
| `iomap.c` `llfs_alloc_block` | 同上 | 同上(block bitmap を txn 経由に) |
| `inode.c` `llfs_add_dirent` | dir bh を `mark_buffer_dirty`+`brelse` | `llfs_txn_log` 経由に |
| `inode.c` `llfs_write_inode` | itable bh を `mark_buffer_dirty`+`brelse` | `llfs_txn_log` 経由に |
| 操作の入口/出口 | — | トランザクション境界(`llfs_txn_begin`/`llfs_txn_commit`)を設ける(§8.10) |

### 8.9 トランザクション境界(コミット契機)

**Phase 1(推奨・最単純)**: **操作ごとの同期コミット**。
`llfs_create` の入口で `txn_begin`、その操作が触る全メタ(bitmap/itable/dir)を `txn_log`、
出口で `txn_commit`。書き込み割当系(`iomap_begin` の block 確保 + `write_inode`)は、
`sync`(= `writepages`/`write_inode`/`sync_fs`)契機で動く現状経路を 1 トランザクションにまとめる。
遅いが正しさが明快で、`write-loop`+`sync` のテストに最適。

**Phase 2(将来)**: `->sync_fs` で**バッチコミット**(running transaction にメタを溜め、
`sync_fs`/journal full/unmount でまとめて commit)。スループット向上。循環ログ化(§8.10)とセット。

### 8.10 段階的実装計画

- **Phase 0**: オンディスク形式追加(SB フィールド・`mkfs.py`・`llfs.h` 構造体)。journal は
  ゼロ(空)。`llfs_journal_recover` は「空なら何もしない」だけ実装。既存動作に影響なし。
- **Phase 1**: §8.5–8.6 の同期 redo ジャーナルを実装。メタ更新を全て `txn_log` 経由に。
  `llfs_create` を 1 トランザクション化。
- **Phase 2**: リカバリ replay(§8.7)を実装し、`write-loop` + 電源断 → 再マウントで
  一貫性が保たれることを確認。
- **Phase 3(任意)**: `sync_fs` バッチコミット、循環ログ(wraparound + head/tail を journal
  superblock に永続化)、data=ordered(データを先に flush してからメタ commit)。

### 8.11 クラッシュシナリオ検証(設計の自己検証)

`llfs_create`(inode bitmap + itable + dir block の 3 ブロックを 1 txn)で電源断した場合:

| 断点 | ジャーナル状態 | リカバリ動作 | 結果 |
|---|---|---|---|
| descriptor 書込中 | desc magic 不定/未完 | 無視 | 旧状態(ファイル未作成)で一貫 |
| data 書込中 | commit 無し | 無視 | 同上 |
| commit 書込中(torn) | seq 不一致 | 無視 | 同上 |
| commit 成立後・checkpoint 前 | コミット済み | replay | 新状態(3ブロック全反映)で一貫 |
| checkpoint 途中 | コミット済み | replay(冪等上書き) | 新状態で一貫(部分反映を補完) |
| クリア後 | desc magic=0 | 無し | 新状態で一貫 |

いずれも **bitmap・itable・dirent が互いに矛盾しない**(リーク/クロスリンク/ダングリング無し)。

### 8.12 既知の制約・留意

- **データはジャーナルしない**(§8.2)。`fsync` 後のデータ永続も厳密には保証しない(メタのみ)。
  data=ordered は Phase 3。
- **同期コミットは遅い**(Phase 1)。`write-loop` は毎回 `sync` するので顕著。正しさ優先の段階。
- **flush の確実な発行**が前提。`sync_dirty_buffer` は I/O 完了待ちはするが、デバイスキャッシュの
  フラッシュには `blkdev_issue_flush(sb->s_bdev)` を各バリア点で明示的に呼ぶこと(QEMU/実機の
  ライトバックキャッシュ対策)。これを怠ると WAL 順序がデバイス内で崩れ得る。
- 単一スレッド前提のロックレス実装。並行マウント操作は対象外。
- §7 の「`blocks` カウント不統一」「`bno << SHIFT` の 32bit 演算」等の既存沼は本設計と直交。
  ジャーナル化の前後で別途修正してよい。
