# xinu-rpi4 — OS としての改良余地アセスメント

**対象**: Pi 4 カーネル（`compile/make pi4` → `kernel8.img`）
**基準コミット**: `ccb6437` (proc: harden tickless one-shot against total hang)
**作成日**: 2026-07-20
**規模**: `.c/.h/.S` 130 ファイル、約 31.4k 行（`extern/` `obj/` `references/` を除く）、195 コミット

この文書は、Pi 4 カーネルを「OS として」評価し、改良余地を影響度と工数で順位付けしたものである。
スケジューラ／メモリ／ネットワーク・ドライバ／FS・ツーリングの 4 領域を精読した結果をまとめている。
重大なものは実際にソースで裏取りし、そのうち 5 件は本アセスメントと同日に修正・実機検証済み
（→ [実施済みの改良](#実施済みの改良-2026-07-20)）。

行番号はアセスメント時点のもので、以後の編集でずれる。関数名を併記しているのでそちらを頼りにすること。

---

## 総評

**上半分（プロセス／IRQ アーキテクチャ）は本物、下半分（メモリ安全性・ネットワーク・FS）はデモ級**という
非対称な成熟度にある。

評価できる点:

- **ISR の中で重い処理をしていない**。net プロセス + app ワーカー + aipl プロセスを
  park/kick で分離し（`loader/main.c:206-269`）、タイマ ISR はティック更新と RT スリーパの
  起床のみ、GENET ISR は計数・自己マスク・ack のみ。HTTP 処理は必ずプロセス文脈で走る。
- **フォルト後も生き延びる**。`recover_spin()`（`system/exception.c:61`）が AIPL ヒープロックを
  解放し IRQ を再開してスピンするので、同期例外のあとも HTTP が応答し `/fault` で原因を読める。
- **MMIO プローブが箱を殺さない**。`safe_mmio_read32/write32`（`system/exception.c:79-104`）が
  `__builtin_setjmp` でガードし、ゲートされた PCIe レジスタへの書き込みが -1 を返すだけで済む。
- ヘッダコメントの質が高い。`device/sd/sd.c:73-86`（74 クロックの電源投入ウィンドウ）、
  `fs/fat32.c:180-184`（クラスタ確保の off-by-2）など、根本原因が文章として残っている。

いずれも「1 回のハングが電源再投入 1 回」という開発コストへの対処として設計が筋道立っている。

弱いのは、**落ちないので気づけない類のバグ**が各層に残っていること。以下はその棚卸しである。

---

## Tier 0 — 実害があり修正コストが小さいもの

すべてソースで裏取り済み。**本アセスメントと同日に全件修正した**（詳細は末尾）。

### ① ヒープ分割の 8 バイト境界オーバーラン — `mem/memory.c` `getmem()`

```c
if (curr->mlength > nbytes) {              /* ← 残り 8 バイトでも成立 */
    struct memblk *leftover = (curr + nbytes);
    leftover->mnext = ...; leftover->mlength = ...;   /* 16 バイト書く */
```

`struct memblk` は 16 バイト、`ROUNDMB` は 8 バイト粒度。残余がちょうど 8 バイトのとき、
**返却したブロックの隣に 8 バイトはみ出して書き、偽の free ノードをリストに繋ぐ**。
以後ヒープが静かに壊れる。

同型の欠陥が `freemem()` 側にもあった（8 バイト領域に 16 バイトのヘッダを書く）。
こちらは目視では見落としており、ホスト側のランダム churn テストが検出した。

### ② ヒープが IRQ／プリエンプション非安全 — `mem/memory.c`

`getmem`/`freemem`/`kmalloc`/`kfree` に critical section が一切ない。
プリエンプションは実際に有効化される経路がある（`/netpreempt`, `/rtos-jitter`, `preempt_demo`）。
`proc_create` は `getmem` を **irq_save の外**で、`proc_kill` は `freemem` を **中**で
呼んでおり（`system/proc.c:207` vs `:422`）、この非対称さ自体が症状である。

### ③ ICMP エコー応答からのメモリ情報漏洩 — `system/net_responder.c:210-234`

```c
int total_len = ip[2]<<8 | ip[3];
if (icmp_len < 8 || total_len + 14 > 1518) return 0;   /* len と比較していない */
int reply_len = 14 + total_len;
for (int i = 0; i < reply_len; i++) tx_reply[i] = frame[i];
```

`total_len` を**実受信長 `len` と照合していない**。60 バイトの ICMP に `total_len=1400` を
書いて送ると、RX リング上の隣接メモリ（＝直前のパケット）約 1.3 KB を攻撃者に送り返す。

同型の欠陥が `system/tcp_server.c:2537-2541`（`data_off` も未検証。`data` がフレーム外を指し、
偽の `data_len` が `peer_seq` を desync させる）。
なお IP／TCP／UDP／ICMP のいずれもチェックサムを検証していない。

### ④ SYN 4 発で恒久 DoS — `system/tcp_server.c` `struct tcp_conn` / `alloc_conn()`

`struct tcp_conn` にタイムスタンプがなく、状態は受信パケットでしか進まない。再送タイマもない。
ACK を返さない SYN を `NCONN`(=4) 本打つと**全スロットが SYN_RCVD で永久に固まり、
再起動まで HTTP が死ぬ**。`alloc_conn()` は LISTEN/CLOSED しか回収しない。

### ⑤ `cat /microsd/xxx` が物理アドレス 0 を読む — `shell/fscmd.c:127`, `loader/main.c:770-774`

FAT32 のファイルを VFS に登録する際 `node->size` は入れるが `node->data` は NULL のまま
（意図的、コメントあり）。`cat`/`cp`/`mv` はそれを無条件に deref する。
アドレス 0 が RW 識別マップ（`system/mmu.c:200`）なのでフォルトせず、
**低位 RAM の中身を表示・コピーする**。`ls` はもっともらしいサイズを出すので気づけない。

### ⑥ W^X が壊れている — `system/mmu.c:191,214`

`kern_2mb = _start & ~2MiB = 0x0` なので、属性を分ける L3（4 KiB ページ）層が覆うのは
`0x0–0x200000` だけ。実際の `_data = 0x268000`。つまり
**`.rodata` の約 416 KB と `.data`/`.bss` 全部（＝全 DMA リング、`proctab`、`smp_stack`）が
2 MiB ブロックの RW+X** になっている。イメージが 2 MiB を超えた時点で静かに劣化したもので、
ビルド時アサートもない。

---

## Tier 1 — 設計上の本丸

### ⑦ 優先度プリエンプションが実質ラウンドロビン — `system/proc.c` `proc_resched()`

`proc_resched()` が**走行中プロセスを ready に戻す前に最良候補を pop する**（`:283` vs `:292-295`）
ので、現在プロセスの優先度が比較に参加しない。prio=50 の RT タスクが prio=5 の hog に
叩き落とされる。`g_dbg_hi_dispatch` カウンタ（`:124`）は、これが追われていた形跡である。

関連して、NULLPROC は決してプリエンプトされず（`:403`）ready リストにも載らない（`:292`）。
NULLPROC は**シェル／ウィンドウマネージャのループそのもの**なので、対話文脈は純粋に協調的で、
`net_yield_tick` の明示 `proc_yield()`（`loader/main.c:295`）でしか進まない。
さらに AIPL アクター実行中はプリエンプションが全面的に無効化される（`proc.c:401`）。
つまり主要ワークロードでは、このカーネルは協調型である。

**修正は数行**。P1（RTOS 化）の看板そのものなので優先度は高い。

### ⑧ ready リストが O(n)、タイマ ISR が毎回 proctab を 2 周

- `ready_pop()` は最大 prio の線形探索（`system/proc.c:107-126`）。`NPROC=2048`、
  N-Queens で約 1300 アクター、しかも **D-cache は意図的に OFF**（`system/mmu.c:263`）なので
  1 ノード＝1 DRAM 往復。
- `proc_next_delay_us()`（`:342-355`）と `proc_timer_tick()`（`:377-390`）が同一 ISR で
  2048 エントリを 2 周。最大 5 kHz。`PROC_TICK_MIN_US` のコメント（`:337-341`）自身が、
  この ISR がシステムを餓死させうると認めている。

→ 優先度別 FIFO + ビットマップ（`CLZ`）で O(1) 化、スリープはソート済みデルタキューへ。

### ⑨ タイムベースが壊れている

`timer_ticks()` は**可変長のワンショット発火回数**であって時計ではない。にもかかわらず
4 つのサブシステムが固定周期として扱っている:
`actorproc.c:150,282,335`（`* 10` = 100 Hz 前提）、`cc/cc.c:514`、
`device/usb/uspi_glue.c:95-100`、`device/usb/xhci/xhci.c:1119-1476`。
アクター GC の経過時間もキーリピートも約 10 倍ずれ、しかも非線形。
`CNTPCT_EL0` 由来の真の `now_ms()` は既に 2 箇所にある（`proc.c:313-319`, `tcp_server.c:789`）。

### ⑩ D-cache OFF が最大の性能レバー、ただし解放には DMA 規律が要る

`SCTLR.C=0` は SMP コヒーレンシを「全アクセスが RAM に届く」で担保する設計判断
（`include/smp.h:10-12`）で理屈は通っているが、4 コア分の D-cache を捨てている。
`DCACHE_ON` を有効にする前提として:

| ドライバ | cache maintenance |
|---|---|
| mailbox (`device/mbox/mbox.c:51-53,85-87`) | 正しい |
| video (`device/video/video.c:247-249`) | 正しい |
| smp mailbox (`system/smp.c:47-50,110`) | 正しい |
| JIT (`cc/cc.c:723-735`) | 正しい（`dc civac` + `ic iallu`） |
| wifi (`device/wifi/wifi.c:2138-2141`) | 部分的 |
| **genet** (`device/genet/genet.c`) | **`dc` 命令ゼロ**（`dsb sy` のみ） |
| **xhci** (`device/usb/xhci/xhci.c`) | **`dc` 命令ゼロ**（`dsb sy` のみ） |

規律は存在するのに 2 大 DMA ドライバだけ抜けている。`DCACHE_ON` は現状どのターゲットも
定義しておらず、実質デッドコードである。

### ⑪ セマフォが偽物、`network/` が丸ごとデッドコード

`system/xinu_compat.c:86-102` の `wait()`/`signal()` はカウンタの増減のみで、
**キューなし・ブロックなし**。`struct sement.queue` は宣言されて未使用。
`sleep()` は即 return、`yield()` は yield しない、`create()` はスレッドを作らない。

その結果、`network/**`（ARP/IPv4/ICMP/netaddr 約 1500 行）は**完全にデッドコード**である。
`netUp`/`netRecv`/`ipv4Recv`/`arpRecv` の呼び出しは `network/` の外にコメントとしてしか存在しない。
それでも `compile/Makefile:47-49` はビルドし続けている。
生かす（本物の netif + ブロックするセマフォ）か消すかの決断が要る。

なお、ネットワークスタックは現在 **4 系統が並存**している:
`system/net_responder.c`（ARP+ICMP）、`system/dhcp_client.c`、`system/tcp_server.c`、
`device/wifi/wifi.c:1238-2100`（ARP+ICMP+DHCP+DNS+NTP+TCP+AODV を独自に再実装）。

### ⑫ スタックガードが皆無

カナリアも guard page も高水位マークもない。一方 IRQ エントリは **768 バイトのフレーム**
（GPR 31 本 + q0-q31 全部、`system/exception_vectors.S:97-131`）を割り込まれたプロセスの
スタックに積み、`proc_create` は 1024 バイトのスタックを受け付ける（`proc.c:192`）。
NULLPROC に至っては `sp = _start = 0x80000` で下方向に firmware spin table へ伸びる、
長さ未知のスタックである（`loader/boot.S:115-116`、`stkbase=0, stklen=0`）。

`struct procent` は `stkbase`/`stklen` を持っている（`include/proc.h:47-48`）ので、
カナリアは 10 行程度で入る。

### ⑬ 2 GB のうち 1 GB が使えない

`HEAP_END=0x40000000` が `compile/Makefile:90` にハードコードされ、しかも
`0x40000000-0x7FFFFFFF`（実 DRAM）を **Device-nGnRnE でマップ**しているので
（`system/mmu.c:221-228`）触ることすらできない。
mailbox の `GET_ARM_MEMORY` か、既にパース済みの DTB `/memory` から取るべき。

### ⑭ TCP に再送も分割もない

- **再送なし**。`struct tcp_conn` に RTO タイマもタイマフィールドもない。
- **ウィンドウなし**。送出ウィンドウは `0x2000` 固定、相手の広告ウィンドウは読まない。
- **順序外の再組み立てなし**。`seq == peer_seq` のときだけ追記し、他は黙って捨てる。
- **分割なし**。`tcp_send()` は 1 フレーム超を拒否するのに、`tcp_app_flush()` は
  最大 16384 バイトを 1 回で渡す。**約 1464 B を超える HTTP 応答は丸ごと落ちる**のに
  `g_app_served++` は回り FIN も出るので、クライアントには「0 バイトの正常終了」に見える。
  `/fb` だけ手作業で 1200 B にチャンクして回避してある。
- **ISN が `0xDEADBEEF` + 接続ごとに `0x100`** で完全に予測可能、かつ ESTABLISHED で
  ACK のウィンドウ検査をしないので、経路外からの注入が容易。

### ⑮ GENET TX が 50 ms のビジーループ

`genet_tx_frame()`（`device/genet/genet.c:902-952`）は `TDMA_CONS_INDEX` を最大 50 ms
ポーリングする。yield しない。ARP 応答も ICMP 応答も ACK も HTTP 応答もこのループを通る。
256 個ある TX ディスクリプタのうち、常に 1 個しか使っていない。しかも送信バッファは
`tx_buf_pool` 1 枚を全呼び出し元で共有し、各呼び出し元も自前の `tx_frame[1518]` を持つので
**パケットごとに 2 回コピー**している。

加えて `genet_rx_poll()` は**ディスクリプタのエラー／status ビットも DMA_OWN も SOP/EOP も
検査せず、`length`（12 ビット、最大 4095）を `RX_BUF_LENGTH`(2048) にクランプもしない**。
`CMD_PROMISC` が常時 ON で、フィルタはソフトウェアの IP 比較のみ。

### ⑯ DHCP が飾り

`dhcp_client.c:340-352` は BOUND まで到達してリースをログに出すが、`tcp_set_ip()` の
呼び出し元がゼロで、`net_responder.c` には IP セッタすら存在しない。
IP は `192.168.3.100` のハードコード（`net_responder.c:23-26`, `tcp_server.c:121`）。
T1/T2 更新もリース失効も DECLINE/RELEASE もない。

### ⑰ ユーザ空間が存在しない

全てが EL1 の単一アドレス空間で動く。`TCR.EPD1=1` で TTBR1 は無効（`system/mmu.c:240`）、
ASID なし、`SP_EL0` 不使用、**`svc` 命令はツリー内に 1 つもない**。
シェルは 51 個の C 関数ポインタの線形探索（`shell/shell.c:926-978`）で、
ハンドラはカーネル内部を直接呼ぶ。`cc/` が JIT したコードも EL1 特権のまま実行可能ヒープ上で走る。

Tier 0 の①⑤⑥⑫が全て「EL0 がないことの緩和策」である以上、いずれ避けられない分岐点である。
ただし `/mmio-write` や `/chainload` が**無認証で任意ハードウェア書き込み・任意コードロード**を
提供している現状では、まずネットワーク面を締めるほうが先である。

---

## Tier 2 — ファイルシステムとエンジニアリング実践

### ⑱ FAT32 にキャッシュがなく、共有 static が無防備

- `fat32_next_cluster()`（`fs/fat32.c:96-107`）は **1 エントリごとに 512 バイト読む**。
  N クラスタのファイルを辿るのに FAT 読みだけで N 回の SD アクセス。
- `fat_find_free()`（`:290-302`）はクラスタ 1 個試すごとに 1 セクタ読む → 書き込みは O(クラスタ数²)。
- `static unsigned char scratch[512]`（`:19`）を `fat32_next_cluster` / `fat_set_entry` /
  `fat_find_free` / `fat32_create_file` / `fat32_write_file` が共有。
  プリエンプティブなスケジューラの下で `sdmount` プロセス・`kexec_tick`・AVM ストリーミングから
  到達しうるが、**ロックは一切ない**。
- **一貫性の保証がない**。`fat32_write_file_full` は新クラスタを確保する**前に**旧チェーンを
  解放するので、途中で電源が落ちるとディレクトリエントリが解放済みクラスタを指す。
  FSInfo も更新しないので、Linux/macOS でマウントすると空き容量が狂う。
- 削除・mkdir・truncate・rename・LFN なし。ルートディレクトリのみ（パス解決がない）。

### ⑲ VFS が抽象化になっていない

`include/vfs.h:22-32` は単一の具象 `vfs_node_t`。`file_ops`/`vnode_ops` の関数ポインタも
マウントテーブルも inode もファイルディスクリプタも `open/close/seek` もない。
`vfs_write` は**オフセット引数を持たず**ファイル全体を置き換える。
FAT32 は完全に別 API で、VFS には配管されていない。

### ⑳ 自動テストが実質ゼロ

これが最大の構造的ギャップである。31k 行・195 コミットに対して:

- `make qemu-smoke` は出力を `tee` するだけで**何もアサートしない**。
  **しかも現在リンクエラーでビルドすら通らない**（`sd_last_int` / `xhci_msd_*` が未定義。
  `ccb6437` 時点で既に破綻。→ 2026-07-20 に `test/host` を追加、後述）
- `examples_http/test.sh` は `192.168.3.100` 固定の curl 7 発、期待値比較なし
- CI なし（`.github/` も `.gitlab-ci.yml` もない）
- `panic()` も `assert()` もない。`-Wall` のみで `-Wextra`/`-Werror` なし

`fs/fat32.c` は read/write コールバックを受け取る（`fat32.h:26-27`）ので、
FAT32 イメージファイルに対する**ホスト側テストが新しい抽象化なしで今すぐ書ける**。
`fs/vfs.c` と `cc/` も同様。ここが投資効率の最も高い領域である。

### ㉑ 重複と腐敗

- `str_eq`/`puts_dec`/`puts_hex` の near-duplicate が約 21 箇所
  （`shell/shell.c:56`, `shell/fscmd.c:15`, `fs/vfs.c:45`, `cc/cc.c:56`,
  `system/kexec.c:18`, `device/video/avm.c:32` …）。
  「各ファイルが自前で持つのがハウススタイル」というコメント（`fscmd.c:12`）は再考の価値がある。
- アクターランタイムが 3 実装並存（`system/actor.c` / `system/actorproc.c` / `avm.c` 内蔵）。
- 陳腐化したコメント: `fat32.h:1` "read-only FAT32 reader"（write 実装済み）、
  `proc.h:12` "No preemption yet"（実装済み）、`shell.c:966` "no scheduler yet"、
  `loader/main.c:781` "no USB mass-storage driver yet"（3 行下で使っている）。
- デッドコード: `device/usb/dwc2.c`、`include/qemu_repro_src.h`、`repro` コマンド、
  ビルドルールから参照されない `examples/` の 11 ファイル。
- **`make install_pi4` は地雷**。`config.txt` を上書きして `total_mem=2048` を落とす。
  この事実は `NEXT_SESSION_PI4.md:31` にしか書かれておらず、Makefile 自身は警告しない。

---

## 推奨着手順序

| 順 | 内容 | 効果 | 工数 | 状態 |
|---|---|---|---|:---:|
| 1 | ①②（ヒープ分割 + irq_save） | クリティカル | 30 分 | ✅ 完了 |
| 2 | ③④（パケット長検証 + half-open reap） | リモート情報漏洩 / DoS | 半日 | ✅ 完了 |
| 3 | ⑤（`node->data` NULL ガード） | サイレント破壊の代表 | 半日 | ✅ 完了 |
| 4 | ⑦（優先度プリエンプション修正） | RTOS の看板そのもの | 1 時間 | 未着手 |
| 5 | ホストテスト基盤 + CI | 以降の全変更の安全網 | 半日 | ◐ `test/host` 追加済 |
| 6 | ⑧（ready キュー O(1) + sleep キュー） | アクター系の実測が動く | 1〜2 日 | 未着手 |
| 7 | ⑥⑫（W^X 修正 + スタックカナリア） | サイレント破壊の封じ込め | 1 日 | 未着手 |
| 8 | ⑩（genet/xhci の DMA 規律 → `DCACHE_ON`） | **単体で最大の性能改善** | 2〜3 日 | 未着手 |
| 9 | ⑭⑮（TCP 分割・再送、GENET TX リング） | HTTP が 1.4 KB で壊れなくなる | 2〜3 日 | 未着手 |
| 10 | ⑬⑯（2 GB 開放、DHCP 適用）、⑪（`network/` の去就） | 中期 | 数日 | 未着手 |
| 11 | ⑰（EL0/ユーザ空間分離、SVC 境界） | OS としての次の段階 | 大 | 未着手 |

---

## 実施済みの改良 (2026-07-20)

Tier 0 の 5 件を修正し、実機（Pi 4 @ `192.168.3.100`）で検証した。
ビルド md5 `74d3ff7f`（修正前 `cfeeb18c`）。

### 修正内容

| # | 修正 | ファイル |
|---|---|---|
| ① | `MEM_ALIGN` を 8 → **16**（= `sizeof(struct memblk)`）。分割の余りは 0 か 16 以上にしかならず、`getmem` のはみ出しと `freemem` のはみ出しが**構造的に消える**。副次的に AArch64 が要求する 16 バイトスタックアラインメントも満たされる | `include/memory.h`, `mem/memory.c` |
| ② | `getmem`/`freemem`/`mem_largest_block`/`mem_free_block_count` を `irq_save`/`irq_restore` で保護 | `mem/memory.c` |
| ③ | ICMP と TCP の長さ検証。`total_len`・`data_off` を実受信長と照合し、フレーム外を指す `data` と偽 `data_len` を排除 | `system/net_responder.c`, `system/tcp_server.c` |
| ④ | アイドル接続リーパー `tcp_conn_reap()`。half-open 5 秒 / ESTABLISHED 60 秒でスロット回収。RX ティックから呼ぶ。`tcpstat` に `reaped=` を追加 | `system/tcp_server.c`, `loader/main.c` |
| ⑤ | `cat`/`cp`/`mv` の `node->data` NULL ガード | `shell/fscmd.c` |

### ホストテスト基盤の追加 — `test/host/`

```sh
make -C test/host run
```

カーネルの純ロジックをビルドマシン上で直接走らせる枠組み。`mem/memory.c` をそのまま
`#include` し、`critical.h` だけホスト用スタブ（`test/host/critical.h`）で差し替える。
インクルード順で実カーネルヘッダを使うので、テストと本番が同じ定義を共有する。

`test/host/memtest.c` が検証するもの:

1. 分割の余りがヘッダ未満になるケース
2. 最小サイズ確保の解放（`freemem` 側の同型バグ）
3. 通常の分割 + 合体のラウンドトリップ
4. 20000 回のランダム churn（97 回ごとに全不変条件を検査）
5. 不正な free（NULL / サイズ 0 / ヒープ外 / 二重解放）の拒否

**この枠組みは実際に仕事をした**。目視では①の `getmem` 側しか見つけておらず、
`freemem` 側の同型バグは churn テストが検出した。

歯があることの確認として、修正前のツリー（`ccb6437`）に同じテストを掛けると **5 件 FAIL**、
修正後は全 PASS になる。

### 実機検証結果

| 項目 | 結果 |
|---|---|
| ICMP（③のリグレッション確認） | `ping` 5/5 応答、パケットロス 0% |
| HTTP（③の TCP パス） | `GET /`, `/ticks` 正常。`txfail=0` |
| リーパーの誤検出（④） | 正常接続 10 本で `reaped=0`、`drop_syn=0` |
| **リーパーの DoS 解除（④）** | アイドル接続でスロットを占有 → **`reaped=3`**、全スロット LISTEN に復帰 |
| ヒープ（①②の `MEM_ALIGN` 16 化） | `mem`: free 977733 KiB、**free blocks = 1**（完全合体）、largest = 全域 |
| `cat` ガード（⑤） | **未検証**。`/microsd`・`/sd` が空でマウント未成立のため到達不能（既知症状） |

### 判明した既存の問題

- **`make qemu-smoke` はビルドすら通らない**。`sd_last_int` / `xhci_msd_*` が未定義でリンクエラー。
  `git stash` して `ccb6437` で確認したので今回の変更による回帰ではなく、元から壊れている。
  つまりリポジトリ唯一の「テストらしきもの」は動作しておらず、実機確認が唯一の検証手段だった。
  `test/host` の追加はこの穴を部分的に埋める。
- `/microsd` と `/sd` が空。SD の FAT32 列挙が成立していない
  （`NEXT_SESSION_PI4.md:123` の既知症状と一致）。⑤の検証を阻んでいる。
