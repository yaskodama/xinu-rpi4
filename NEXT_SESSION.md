# NEXT_SESSION — xinu-rpi5

## ✅ 2026-05-27 (session 2) — HTTP actor gateway + Mac AIPL→Xinu actor 完動

**実機で「Mac の AIPL アクター → Xinu のアクターへ TCP/HTTP メッセージ」が完動。**
`python3 src/python-aipl/samples/xinu_http_actor_demo.py 192.168.3.100` →
`counter.bump→3 / add(40)→43 / get→43`、`store.set(7).add(35)→42`（値整合）。
シリアルに `SYN→SYN+ACK→ESTABLISHED (await HTTP request)→http: served request,
FIN sent`。**現在 SD カードに焼かれているカーネル: md5 `d466b1b`。**

### この session で land したもの（**未 commit**, 別途コミット予定）
1. **flicker 対策 = ダブルバッファ** (`device/video/video.c`, `wm.c`, `include/video.h`)
   - `fb_draw`(描画先)/`fb_back`(裏)/`video_present()`(裏→表一括コピー)、`wm_run` で
     `video_enable_backbuffer()`+毎フレーム `video_present()`。確保失敗時は直接描画。
   - 実機での flicker 解消は未確認（HTTP 機能を優先してテスト）。
2. **HTTP actor gateway** (`system/tcp_server.c`)
   - TCP listener を HTTP/1.0 化。`GET /` / `GET /api/actors` /
     `GET /send?to=N&m=<bump|add|set|get|reset>&arg=<n>`。ESTABLISHED の最初の
     data segment を request とみなし `http_build()` でルーティング、JSON 応答+FIN。
     port 80。libc 無いので s_put/s_putdec/q_param を内製。
3. **最小 actor 層** (`system/actor.c` + `include/actor.h`)
   - ACTOR_MAX=8、各 int field[4]+name。demo: id0=counter, id1=store。
     `actor_message(id,method,arg,*out)` 同期配信。**将来の AIPL codegen 受け皿**。
4. **Mac 側 AIPL `http://` scheme** (abclcp-project 側 `src/python-aipl/aipl_remote.py`)
   - `_is_xinu_http()`/`_xinu_http_send()`、remote_send/remote_call_sync を
     `GET /send?...` に翻訳。明示 `http://` prefix=Xinu gateway / bare host:port=
     AIPL JSON gateway。デモ: `src/python-aipl/samples/xinu_http_actor_demo.py`。
5. **★ `-mstrict-align` を COMMON CFLAGS に追加** (`compile/Makefile`) — **最重要**
   - actor_init() が `gratuitous ARP done` 直後でハングした真因 = GCC -O2 が
     `set_name`/`field[]`ゼロ化を不整列ワイドストアにマージ → **MMU-off
     Device-nGnRnE で data abort**（TCP seq/ack と同 class）。`-mstrict-align` で
     GCC が不整列アクセスを一切出さなくなり**この種の地雷を kernel 全体で根絶**。
     QEMU smoke PASS、退行なし。**A2 (AIPL 移植) の必須土台**。

### 再開時のテスト手順
```sh
# Mac
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
curl http://192.168.3.100/api/actors
curl 'http://192.168.3.100/send?to=0&m=bump'
python3 src/python-aipl/samples/xinu_http_actor_demo.py 192.168.3.100
# シリアル: screen /dev/cu.usbserial-1120 115200   (抜ける: Ctrl-A K y)
# ビルド/焼き: cd compile && make pi4 ; cp kernel8.img /Volumes/bootfs/ ; sync
```

### 次の作業 = A2（本物の AIPL アクターを Xinu 上で）
`src/c_translator.ml --xinu`（abclcp-project、xinu-raz 向けの AIPL→C codegen）を
**xinu-rpi5 の actor/mailbox API 向けに retarget**し、.abcl のアクターを Xinu で
動かす。現状の Xinu actor は `system/actor.c` のネイティブ実装（object table 風で
受け皿になっている）。`-mstrict-align` 済なので不整列地雷は回避済み。

落とし穴メモ: ① 全 multi-byte パケット/構造体アクセスは `-mstrict-align` で安全化
済（個別 volatile はもう不要だが既存のは残置）② Pi 4 シリアルは uart.c の
GPIO14/15→ALT0 明示マックス必須 ③ Mac の static ARP は失効するので毎回 `arp -s`。

---

## ✅ 2026-05-27 — Pi 4 NET-G TCP 実機完走（解決）

`nc 192.168.3.100 8023` で **3-way handshake → ESTABLISHED → greeting
"Hello from xinu-rpi5 ..." 送信**を実機で確認（シリアルログに `tcp: SYN ...
-> SYN+ACK` / `tcp: ESTABLISHED`、以後も RX が流れ続け＝ハング解消）。

**長らくの「nc で全停止」の真因 = unaligned 32bit read の data abort:**
- `tcp_handle_packet` の `seq`/`ack` 読み `(tcp[4]<<24)|(tcp[5]<<16)|...` を
  GCC -O2 が big-endian 32bit load idiom と認識し単一 `ldr` にマージ。TCP seq は
  フレームオフセット **38（4 バイト境界外）**。**MMU off で DRAM が
  Device-nGnRnE** なので unaligned word load が **data abort → 無限ハング**
  （Frame/rx/ping 全停止）。ICMP には 32bit アライン外読みが無く ping は無事だった。
- **修正**: `system/tcp_server.c` の解析ポインタ `ip`/`tcp`/`data` を
  `const volatile unsigned char *` に（GCC のワイドロード・マージ禁止＝厳密バイト
  単位ロード）。net_responder の TX-frame stores と同じ Device-nGnRnE 対策の RX 版。
  同型の罠は `dhcp_client` の `got_xid` 読みにもある（DHCP 有効化時は要 volatile 化）。
- **シリアルが必須だった**: `uart.c` に GPIO14/15→ALT0 を明示ピンマックス
  （`-DGPIO_BASE=0xFE200000UL`）。Pi 4 は firmware が PL011 を BT に取り mini-UART を
  header に出すことがあり、これが無いとシリアルに何も出ない。配線: 黒=GND(pin6) /
  白=pin8(GPIO14 TXD) / 緑=pin10(GPIO15 RXD) / 赤(VCC)=未接続。115200 8N1。
- **デバッグ手法**: `genet_rx_tick` に RX マーカー D/R/r を仕込みシリアルで
  「ハング直前の最後の文字＝`D`（解析中の abort）」を特定 → 解決後マーカーは除去済み。

→ 以下は 2026-05-25 時点の記録（root cause 認識が「リング飽和」だったが、実際は
  上記 unaligned read だった。リング拡張/bounded loop 等は改善として残置）。

---

## NET-G TCP を dormant → **LIVE** 化（2026-05-25 記録）

前回 (`d9e9b81`) は「`tcp_handle_packet` を rx_tick から呼ぶだけで ICMP
responder が止まる」という未解決バグで TCP dispatch を OFF にしていた。
今回その**根本原因を特定して修正**し、TCP listener を常時稼働にした。

### 根本原因 — RX/TX リングの飽和 → 恒久 wedge

- dispatcher (`genet_rx_tick`) は **WM フレームごとに 1 回**しか走らない
  (`wm_run` が 6 ウィンドウを全描画する ~50ms/frame ≒ 20fps が律速。
  これが ping RTT 900ms の正体でもある)。
- 旧 RX リングは **16 descriptor (32KB) だけ**。`nc` 接続時の SYP burst +
  LAN broadcast + hot path 中の `uart_puts`(framebuffer console は激遅) が
  重なると、50ms の描画中にリングが埋まり、**on-chip RBUF FIFO が overflow
  → RX path が wedge**。ICMP echo request すら受信できなくなるので
  「responder が止まった」ように見えていた。
- TX 側も同様で、`genet_tx_frame` の timeout 時に ring を放置していたため、
  1 本詰まると以後の ICMP reply(TX) も全部詰まる ("TX ring degradation"、
  NET-F の DHCP で観測されていた症状と同根)。

### 修正内容 (commit 予定、未 commit)

| ファイル | 変更 |
|---|---|
| `device/genet/genet.c` | RX ring **16→256** (HW 全 descriptor)。`genet_rx_recover()` 追加 (RBUF flush + 全 desc 再 arm + CONS/SW index 再同期)。`genet_rx_poll` に overrun guard (`prod-cons > ring` で recover)。hot path の per-packet `uart_puts` 全撤去。`genet_tx_frame` timeout 時に ring resync + DMA 再 enable。overrun/recovery/tx_timeout カウンタ + accessor。 |
| `loader/main.c` | `genet_rx_tick` dispatch を `DHCP → TCP → ARP/ICMP` の clean な連鎖に。**`tcp_handle_packet` を LIVE 化**。boot で `tcp_set_mac()` + `tcp_listen(23)`。hot path のデバッグダンプ撤去。 |
| `system/tcp_server.c` | LISTEN で **SYN+ACK を実送出** + `TCP_SYN_RCVD` 遷移 (前回はコメントアウトで handshake 不成立だった)。 |
| `system/net_responder.c` | ICMP/ARP の per-packet ログを先頭 8 件で throttle (steady-state の hot path を高速化)。 |
| `shell/shell.c` | `rxstat` に `overruns= recoveries= tx_timeouts=` を追加。 |

### 副次的に直したビルド破損 (TCP とは無関係、要 commit)

`make clean` したら **コミット済みツリーが元々クリーンビルドできない**ことが
発覚 (Makefile にヘッダ依存追跡が無く、ヘッダ drift しても .c が再コンパイル
されず stale .o で動いていた)。以下を修正し、**pi4 / pi5 / qemu 全 3 変種が
0 error でクリーンビルド**するようにした:

- `include/memory.h`: `ROUNDMB`/`TRUNCMB` マクロを追加 (memory.c が使うが未定義
  だった)。`freemem` 宣言を `void`→`int` (定義と一致)。xinu-raz network 移植が
  `<memory.h>` 経由で要求する `struct memblock` を定義 (include 順で native
  memory.h が優先され incomplete type になっていた、13 ファイルが失敗)。
- `include/genet.h`: 非 pi4 (`#else`) 分岐に全 genet 関数の inert stub を追加
  (pi5/qemu の main.c/shell.c が無条件参照していた)。
- `shell/shell.c`: `_end` のローカル再宣言を削除 (memory.h と型衝突)。

## 検証状況

- ✅ **pi4 / pi5 / qemu フルクリーンビルド 0 error** (`make clean && make all`)。
  kernel8.img 67088 / kernel_2712.img 56400 / kernel_virt.img 56144 bytes。
- ✅ **QEMU virt smoke**: boot / shell / pingpong / procdemo / ps / halt 全 OK。
  `mem` で heap allocator (ROUNDMB/freemem 修正) が正常動作 (`_end` も正値)。
- ⏳ **GENET ネットワークは QEMU では検証不可** (virt に GENET 無し)。**実機
  Pi 4 で flash + ping/nc 検証が必要** ← 次のアクション。

## 実機テスト手順 (Pi 4)

```sh
# 1. ビルド (済) — 念のため
cd /Users/kodamay/projects/xinu-rpi5/compile && make pi4

# 2. SD に焼く
diskutil mount /dev/disk4s1            # bootfs
cp kernel8.img /Volumes/bootfs/
sync && diskutil eject /Volumes/bootfs
#   (または: make install_pi4 SDCARD=/Volumes)

# 3. Pi 4 を起動、Mac 側 ARP 固定 + ping (= 退行していないことを先に確認)
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100                     # ← まず ping が今まで通り通ること

# 4. TCP を試す (今回の本命)
nc 192.168.3.100 23
#   期待: 3-way handshake 成立 → "Hello from xinu-rpi5 (Pi 4, BCM2711)!"
#         → ENTER で echo + 接続 close
#   nc 実行中も別ターミナルで ping が生き続けること (= ICMP 干渉が消えたこと)
```

### 見るべきポイント

- UART0 (シリアルコンソール) に `tcp: SYN from ... -> SYN+ACK` →
  `tcp: ESTABLISHED` → (ENTER 後) `tcp: FIN sent (got newline)` が出る。
- shell で `rxstat` を叩き、`overruns= / recoveries= / tx_timeouts=` を確認。
  - **理想は全部 0**。`recoveries` が少し増える程度なら自己回復が効いている証拠。
  - `tx_timeouts` が増え続けるなら TX ring がまだ不安定 (下記フォローアップ)。

## まだ残っている可能性 / フォローアップ

1. **実機で本当に直ったかは未確認** (QEMU で GENET を出せないため)。もし実機で
   まだ ping が止まるなら、`rxstat` の overruns/recoveries/tx_timeouts の伸び方で
   RX 飽和か TX wedge かを切り分けられる (今回そのための計器を仕込んだ)。
2. **RX を WM フレームより高頻度で drain したい**: 本質的には dispatch が
   20fps に縛られているのが弱点。S1 (GIC + Generic Timer の preemptive) が入れば
   タイマ割り込みから RX を回せて飽和耐性が一段上がる。
3. **DHCP (NET-F)** は依然 dispatch OFF (`dhcp_send_discover` は boot から外して
   ある)。TX ring recovery が入ったので連続 DISCOVER の劣化は緩和されているはず
   — 再挑戦の余地あり。`sudo tcpdump -i en0 -nn 'port 67 or 68'` で OFFER の
   戻り経路を切り分けるのが先。
4. **Makefile にヘッダ依存追跡が無い**問題は未対処。`-MMD -MP` + `-include
   $(DEPS)` を足すと、今回のような「ヘッダ直しても再コンパイルされず stale .o」
   の地雷が消える。次の小タスク候補。

## ファイル位置

```
リポ: /Users/kodamay/projects/xinu-rpi5/   branch: main
変更: device/genet/genet.c, include/genet.h, include/memory.h,
      loader/main.c, shell/shell.c, system/net_responder.c,
      system/tcp_server.c   (全て未 commit)
```

## Round 1 の他の残作業 (参考、優先順は実機 TCP 検証の後)

- **S1 preemptive timer** (GIC + Generic Timer IRQ → preemptive scheduler)
- **X1 AIPL hello** (Py-I or JS-O runtime を xinu-rpi5 上で hello-world)
- **XHCI-B〜H** (Pi 4 USB-A キーボード/マウス: VL805 enumeration)
- **N0 RP1 discover** (Pi 5 に切り替えるなら)
