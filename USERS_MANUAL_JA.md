# Xinu xinu-rpi4 ユーザーズマニュアル (日本語版)

Raspberry Pi 4 (BCM2711 / Cortex-A72, AArch64) 向けの Embedded Xinu 移植版。
同じソースツリーから QEMU `virt` 機にもクロスビルドできる。
本マニュアルは「動かす側」向け — リポジトリの開発ロードマップは `README.md`
を参照のこと。

----------------------------------------------------------------------

## 1. 対象ターゲットとビルド成果物

| ターゲット | ボード / 機種            | 出力イメージ        | UART0 base     |
|------------|--------------------------|---------------------|----------------|
| `pi4`      | Raspberry Pi 4 (BCM2711) | `kernel8.img`       | `0xFE201000`   |
| `qemu`     | `qemu-system-aarch64 -M virt -cpu cortex-a76` | `kernel_virt.img` | `0x09000000` |

load address は `0x80000` (Pi 4) / `0x40080000` (QEMU)。
2 つのイメージは同一ソースから `-D<TARGET>` と異なるリンカスクリプトで作り
分けている。

### 1.1 機能対応マトリクス (重要)

「全ターゲットで動く」訳ではない。 **実機 (Pi 4) でしか動かない機能** が複数
あるので要注意:

| サブシステム            | Pi 4 | QEMU virt | 備考                                         |
|-------------------------|:----:|:---------:|----------------------------------------------|
| PL011 UART0 シェル      |  ✅  |    ✅     | 全機種で動く土台                              |
| 協調的スケジューラ (S0) |  ✅  |    ✅     | `procdemo` / `pingpong`                       |
| プリエンプティブ (S1)   |  ✅  |    ⏳     | Pi 4 のみ 100 Hz tick 稼働 (`ticks`)         |
| HDMI フレームバッファ   |  ✅  |    —      | Pi 4 で実用                                   |
| ウィンドウマネージャ    |  ✅  |    —      | 1280×960 仮想デスクトップは Pi 4 のみ実用     |
| SD カード + FAT32       |  ✅  |    —      | EMMC ドライバが Pi 4 (BCM2711) 向け           |
| Ethernet (GENET)        |  ✅  |    —      | NET-E 完了 (BCM2711 GENET)                    |
| **USB (キーボード/HID)**|  ✅  |    —      | VL805 PCIe xHCI 経由 (USB-2.0 ポート、§10/§11)|
| DHCP / TCP              |  ❌  |    —      | コードは在るが dispatch OFF (§7.4)            |

> ⚠ ソースツリーには `device/genet/`, `device/sd/`, `device/usb/xhci/` 等が
> 同居しており、これらは **Pi 4 (BCM2711) のレジスタアドレスで初期化** される。
> `-DGENET_BASE` 等は Pi 4 ビルドでのみ定義され、QEMU ビルドでは undef になって
> 対応コードがリンクから外れる作り。

### 1.2 起動シーケンス (特殊な部分)

`loader/boot.S` は通常の bare-metal stub よりひと工夫が要る:

1. **Linux ARM64 Image ヘッダが必須**
   - Pi 4 の EEPROM bootloader はファイル先頭から 0x38 オフセットに
     `"ARM\x64"` マジックが無いとカーネルへジャンプしないケースがある
   - `boot.S:46` の `_start:` 直下に 64 バイトのヘッダ
     (code0/code1/text_offset/image_size/flags/magic) を配置
   - これが無いと **HDMI に虹色が出たまま固まる** のが症状
2. **MPIDR_EL1 によるコア固定**
   - `MPIDR_EL1[1:0]` で boot core 以外 (core1/2/3) を `wfe` ループに park
   - S0/S1 の作業で起こすまでは core0 単一動作
3. **スタック設定**
   - `sp = _start (= 0x80000)`、 leex 流儀でカーネル先頭アドレスを SP に
4. **DTB アドレス退避**
   - ファームウェアから `x0` に渡される DTB の物理アドレスを BSS clear の
     間退避し、終わったら `.data` 上の `dtb_addr` に格納
5. **BSS クリア → `kernel_main`**
   - 8 バイト単位で `__bss_start` から `__bss_size` ワード分ゼロクリア
   - その後 `bl kernel_main` で C 側へ。 `kernel_main` から戻れば
     secondary と同じ `wfe` パークへ落ちる

EL のドロップ (EL2→EL1) は自分で実装していない (QEMU も同じ前提)。
config.txt で `armstub` を指定したり、`kernel_old=1` 等を入れると
状況が変わるので注意。

----------------------------------------------------------------------

## 2. 用意するもの

### 2.1 ハードウェア (実機の場合)

- Raspberry Pi 4 (BCM2711)
- microSD カード (FAT32 で `bootfs` パーティションがあるもの)
  - 一番簡単なのは Raspberry Pi OS をいったん焼き、後で `kernel8.img`
    と `config.txt` を上書きする方式
- USB-シリアル変換ケーブル (3.3V、115200 8N1)
  - ヘッダピン 8 (TXD → GPIO14) / 10 (RXD → GPIO15) / 6 (GND) に接続
- ネットワーク機能を使う場合: イーサネットケーブル

### 2.2 ホスト側ソフトウェア

- macOS / Linux
- AArch64 クロスツールチェイン (どちらか):

```sh
# Homebrew (推奨)
brew install aarch64-elf-gcc

# または ARM 公式
brew install --cask gcc-arm-embedded
```

- `qemu-system-aarch64` (QEMU で動かす場合)

```sh
brew install qemu
```

- ターミナルシリアルクライアント (例: `screen`、`minicom`、`picocom`)

----------------------------------------------------------------------

## 3. ビルド手順

ビルドは必ず `compile/` ディレクトリで行うこと (`Makefile` がここを基準に
`../loader/`、`../device/...` を `VPATH` で集める作り)。

### 3.1 基本コマンド

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile

make pi4            # → compile/kernel8.img      (Raspberry Pi 4 実機)
make qemu           # → compile/kernel_virt.img  (QEMU virt)
make                # = make all = pi4 + qemu
make clean          # オブジェクト・全 .img を削除
```

各ターゲットのオブジェクトは別ツリーに分離される
(`obj/pi4/`, `obj/qemu/`) ので、続けて別ターゲットを
ビルドしても再コンパイルは差分のみ。

### 3.2 ツールチェインの自動検出

Makefile は次の順で AArch64 GCC を探す:

1. `$(GCCPATH)/bin/aarch64-elf-gcc` (Homebrew `aarch64-elf-gcc`)
2. `$(GCCPATH)/bin/aarch64-none-elf-gcc` (ARM 公式 `gcc-arm-embedded`)

`GCCPATH` の既定値は `/opt/homebrew`。 別の場所にインストールしたなら:

```sh
make pi4 GCCPATH=$HOME/aarch64/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
```

プレフィックスも上書き可能:

```sh
make pi4 CROSS=aarch64-linux-gnu-
```

### 3.3 主な Make 変数

| 変数         | 既定値                  | 用途                              |
|--------------|-------------------------|-----------------------------------|
| `GCCPATH`    | `/opt/homebrew`         | クロスコンパイラのインストール先  |
| `CROSS`      | (自動検出)              | プレフィックス (`aarch64-elf-` 等)|
| `SDCARD`     | `/Volumes`              | `make install_*` のマウント親     |
| `DEST`       | `$(SDCARD)/bootfs`      | コピー先パーティション            |

### 3.4 ビルド時の主な定義 (CFLAGS で渡る)

| マクロ          | Pi 4             | QEMU virt    |
|-----------------|------------------|--------------|
| `-mcpu`         | `cortex-a72`     | `cortex-a76` |
| `UART0_BASE`    | `0xFE201000`     | `0x09000000` |
| `MBOX_BASE`     | `0xFE00B880`     | (未定義)     |
| `SD_BASE`       | `0xFE340000`     | —            |
| `USB_BASE`      | `0xFE980000`     | —            |
| `GIC_BASE`      | `0xFF840000`     | —            |
| `PCIE_BASE`     | `0xFD500000`     | —            |
| `GENET_BASE`    | `0xFD580000`     | —            |
| `HEAP_END`      | `0x40000000` (1G)| `0x50000000` |
| `SKIP_MBOX`     | —                | 定義 (簡略化)|
| `KERNEL_NAME`   | `"kernel8.img"`  | `"kernel_virt.img"` |
| `BOARD_NAME`    | `"Pi4"`          | `"virt"`     |
| `SOC_NAME`      | `"BCM2711"`      | `"QEMU"`     |

共通 CFLAGS:
`-Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mgeneral-regs-only`

`-mgeneral-regs-only` 指定により大半のコードで FPU/NEON は使わない
(`cc.c` / `llm.c` のみ FP 有効でビルドし、CPACR で EL1 から FPU を解放する)。

### 3.5 何がリンクされているか

`Makefile` の `COMPONENTS` リスト:

```
loader mem system shell fs cc llm
device/uart device/mbox device/video device/sd device/usb
device/usb/xhci device/gic device/timer device/genet device/wifi
network/arp network/net network/netaddr network/ipv4 network/icmp
```

すべて `*.c` / `*.S` を `$(wildcard ...)` で吸い上げる。
Pi 4 専用周辺 (GENET / SD / USB xHCI など) は §1.1 表の通り、Pi 4 ビルドで
のみ実体が動く。QEMU ビルドでは `-DGENET_BASE` 等が未定義のため対応コードは
no-op になる。

### 3.6 リンカスクリプト

| スクリプト             | 対象      | エントリ・ロードアドレス  |
|------------------------|-----------|---------------------------|
| `compile/link.ld`      | Pi 4      | `0x80000`                 |
| `compile/link_virt.ld` | QEMU virt | `0x40080000`              |

どちらも `.text.boot` を先頭に置き、`__bss_start` / `__bss_size`
(8-byte 単位) をエクスポートする leex 風の構成。

----------------------------------------------------------------------

## 4. SD カードへの書き込み

### 4.1 Pi 4

```sh
cd compile
make install_pi4 SDCARD=/Volumes
# → /Volumes/bootfs に kernel8.img と sdcard/config_pi4.txt をコピー
```

`make install` は `install_pi4` のエイリアス。

### 4.2 手動で焼く場合 (例: 別の SD リーダー)

```sh
diskutil mount /dev/disk4s1                # → /Volumes/bootfs
cp compile/kernel8.img /Volumes/bootfs/
cp sdcard/config_pi4.txt /Volumes/bootfs/config.txt
sync
diskutil eject /Volumes/bootfs
```

> ⚠ `kernel8.img` の md5 が SD カード上のものと一致するか必ず確認すること。
> 古いイメージで起動して混乱するのは「あるある」。

### 4.3 SD カードに必要なもの

`kernel8.img` と `config.txt` だけでは起動しない。以下も同じ FAT32
パーティションに置く必要がある:

- Raspberry Pi OS の `bootcode.bin`, `start4.elf`, `fixup4.dat` ほか
  ファームウェア blob 一式
- 通常は Raspberry Pi OS をフラッシュした後、`kernel8.img` と
  `config.txt` だけ上書きするのが最速

----------------------------------------------------------------------

## 5. シリアルコンソールへの接続

USB-シリアル変換ケーブル経由で接続:

```sh
# Mac: screen
screen /dev/tty.usbserial-XXXX 115200

# Mac: minicom
minicom -b 115200 -o -D /dev/tty.usbserial-XXXX

# Linux
sudo screen /dev/ttyUSB0 115200
```

`screen` を終了するときは `Ctrl-a k` → `y`。

電源投入後、約 5 秒以内に次のバナーが出る:

```
================================================
  Xinu Pi4 hello (AArch64, BCM2711, kernel8.img)
  PL011 UART0 @ 0xFE201000, 115200 8N1
  bootstrap: leex-style stub + xinu-rpi4 main
================================================

Round 1 phase B/U done — entering interactive shell.
type `help` for the command list.
xinu-pi4$ _
```

----------------------------------------------------------------------

## 6. シェルコマンドリファレンス

`help` または `?` で常に最新の一覧が見られる。現在のコマンド一覧:

| コマンド | 用途 |
|---------|------|
| `help` / `?` | 登録コマンド一覧を表示 |
| `echo <words…>` | 引数をそのままエコーバック |
| `hello` | 動作確認用の挨拶 |
| `mem` | `__bss_start` / `__bss_end` / `_end` を表示 (link.ld 由来) |
| `peek <hex_addr>` | 32-bit MMIO ワードを読む (例: `peek 0xfe201018`) |
| `uptime` | 生の `CNTPCT_EL0` (汎用タイマカウンタ) を表示 |
| `ticks` | 100 Hz タイマ tick の累積数 (S1) |
| `ps` | コア / EL ステータス簡易表示 |
| `halt` | DAIF マスク + PSCI `SYSTEM_OFF` (QEMU `virt` はクリーン終了) |
| `reboot` | スタブ — 電源再投入待ちでスピン |
| `pwd` `ls` `cd` `mkdir` `rmdir` `touch` `cat` `write` `edit` `rm` `cp` `mv` `tree` | RAM 上のファイルシステム (揮発性。§6.3) |
| `cc <file.c>` | C プログラムをオンデバイスでコンパイル&実行 (JIT → AArch64。§6.3) |
| `aload` / `amsg` | 常駐 AIPL アクタのロード / メッセージ送信 (§6.3) |
| `actordemo` / `selectdemo` | アクタ ping-pong / 名前付きメッセージの選択受信 (§6.3) |
| `vmtest` / `vmdemand` | VA≠PA リマップ / デマンドページング仮想メモリ (§6.4) |
| `llm [prompt]` | 組込み LLM でテキスト生成 (§6.3) |
| `preempt` | タイマ駆動プリエンプティブ RR スケジューラのデモ |
| `pingpong [N]` | AIPL 風 2 アクター協調 PingPong (N=1..50, 既定 5) |
| `procdemo [N]` | 実 ctxsw による 2 プロセスデモ (N=1..30, 既定 5) |
| `usb` | xHCI / DWC2 USB 診断 (Pi 4 のみ) |
| `wifi …` | WiFi + メッシュ — `probe`/`scan`/`up`/`adhoc`/`aodv`/… (§7.5–§7.6) |
| `rxstat` / `tcpstat` | RX リング / TCP listener カウンタ |
| `pan <dx> <dy>` `view` `autopan [on|off]` | ウィンドウマネージャのビューポート操作 |

### 6.1 procdemo の見どころ

`procdemo 3` を実行すると、本物の AArch64 コンテキストスイッチが走る:

```
xinu-pi4$ procdemo 3
procdemo: created pid=1 (ping) and pid=2 (pong), iters=3
---------------------------------------------
  [Ping pid=1] tick 1
  [Pong pid=2] tock 1
  [Ping pid=1] tick 2
  [Pong pid=2] tock 2
  [Ping pid=1] tick 3
  [Pong pid=2] tock 3
  [Ping pid=1] exit at iter 3
  [Pong pid=2] exit at iter 3
---------------------------------------------
procdemo: both processes exited; back in shell.
```

`pid=N` はグローバル `currpid` から実行時に読み出した値なので、
列方向の交互パターン自体がスケジューラが本当に切替えている証拠になる。
スタックは `proc_exit` では回収していない (S1 で IRQ tick 経由で回収予定)。

### 6.2 pingpong との違い

| 観点          | `pingpong`                       | `procdemo`                              |
|---------------|----------------------------------|-----------------------------------------|
| アクター      | 静的 2 つ (`Ping`, `Pong`)       | `proc_create` した実プロセス pid=1, 2   |
| スイッチ      | 単一スタック上のディスパッチ     | `ctxsw.S` による実 callee-saved 保存復元 |
| 停止条件      | 両者の inbox が空                | 両者が `proc_exit()`                    |

### 6.3 オンデバイス・ツール (ファイルシステム / C JIT / アクタ / LLM)

上記のデモに加え、シェルには自己完結したツールチェインが載っており、ボード上でコードを書いて・コンパイルして・実行できます。

- **RAM 上のファイルシステム** — `pwd` `ls` `cd` `mkdir` `rmdir` `touch` `cat` `write` `edit` `rm` `cp` `mv` `tree` がメモリ上の小さなツリーを操作 (揮発性。再起動で消える)。
- **C JIT (`cc`)** — `cc <file.c>` が C のサブセットをネイティブ AArch64 にコンパイルしてその場で実行。同じコンパイラは HTTP の `POST /compile` (本文 = C ソース) からも使えます。
- **AIPL アクタ** — `aload <file.c>` で常駐アクタをロード、`amsg <actor> <method> [arg]` でメッセージ送信。`actordemo` は 2 アクターの ping-pong を実 Xinu プロセスとして実行、`selectdemo` はガード付き受信 (名前付きメッセージを優先) を示します。HTTP の `/actor`・`/send`・`/gc` がアクタ一覧・送信・アクタプール GC を公開。
- **組込み LLM (`llm`)** — 小さな transformer がイメージに焼き込まれており、`llm [prompt]` でオンデバイス生成 (HTTP の `/chat` でも)。

### 6.4 仮想メモリ (`vmtest` / `vmdemand`)

カーネルは MMU 有効 (identity map) + VA `0x80000000`..`0x80400000` (4 MiB, 512 フレームプール) の**デマンドページング窓**で動作します:

- `vmtest` — 同じ物理ページを別の仮想アドレスにマップして変換を実証 (VA ≠ PA)。
- `vmdemand` — デマンド窓の 64 ページに触れる。初回は `0x40` 回のページフォルト (1 ページ 1 回) でリードバック OK、2 回目は `0x0` フォルト (既にマップ済み)。フォルトカウンタは `/fault` (HTTP) で確認。

ローカルでもリモートでも実行可: 例 `curl "http://192.168.3.100/shell?cmd=vmdemand"`。

----------------------------------------------------------------------

## 7. ネットワーク機能 (Pi 4)

`xinu-rpi4` のネットワークドライバは Pi 4 GENET (BCM2711) で動作する。

### 7.1 動作する機能

- ARP request / reply
- ICMP echo (ping)
- 静的 IP / MAC: `192.168.3.100` / `d8:3a:dd:a7:fd:bf`
  (`loader/main.c:707` 付近で設定)
- 生 broadcast / unicast 送信
- RX リング 16-slot

DHCP クライアントと TCP listener はソースに同梱されているが、現在は
リング劣化問題のため `rx_tick` からの dispatch を **OFF** にしてある。

### 7.2 Mac から ping する手順

```sh
# Pi 4 起動後、Mac で:
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
```

RTT は約 900 ms (ウィンドウマネージャの tick 律速)。

### 7.3 シェルからのネット状況確認

```
xinu-pi4$ rxstat
xinu-pi4$ ticks
```

### 7.4 DHCP / TCP の現状

**TCP は有効です。** `system/tcp_server.c` が `rx_tick` から dispatch され、
ポート 80 で HTTP 制御面を提供します (7.7 節)。かつて TCP を有効にすると
ICMP echo reply が止まった問題は解消済みです。

ただし実装は RFC 793 の部分集合です。利用時は以下を前提にしてください:

- 同時接続は **4 本**まで (`NCONN`)。HTTP リクエストの処理自体は一度に 1 本。
- **再送・ウィンドウ制御・順序外の再組み立てがありません。**
- **応答の分割がありません。** 約 1.4 KB を超える HTTP 応答は送出されず、
  クライアントからは「0 バイトで正常終了」に見えます。大きな出力は分割して
  取得してください。
- 無通信の接続は自動回収されます (half-open 5 秒 / 確立済み 60 秒)。回収数は
  `tcpstat` の `reaped=` で確認できます。これにより、応答を返さないクライアント
  が 4 スロットを占有し続けてサーバが無反応になることはありません。

**DHCP は未適用です。** `system/dhcp_client.c` はリース取得 (BOUND) まで
到達しますが、取得したアドレスはスタックに反映されません。IP は
`192.168.3.100` の固定値のままです。家庭用 router の WiFi↔LAN bridge が
DHCP broadcast を片方向しか通さず OFFER が戻らないケースがある点も従来どおり。

### 7.5 WiFi (BCM43455)

Pi 4 はオンボードの BCM43455 WiFi チップを持ちます (§7.1 の有線 GENET とは別系統)。操作はすべてシリアルシェル (`xinu-pi4$`) から行います: ファームウェアを起こし、スキャンし、接続します。**起動時に自動接続はしません。**

| コマンド | 説明 |
| --- | --- |
| `wifi probe` | M0/M1 bring-up: チップにファームウェアをダウンロード (起動ごとに 1 回) |
| `wifi scan` | 近隣 AP を escan |
| `wifi up <ssid> <pass>` | join (WPA2-PSK) + DHCP。常駐 ARP/ICMP 応答を開始 |
| `wifi join <ssid> <pass>` | join のみ (DHCP なし) |
| `wifi dhcp` | DHCP リース取得 |
| `wifi off` | 無線停止 / 切断 |
| `wifi ping <a.b.c.d> [count]` | ICMP echo クライアント |
| `wifi serve [secs]` | ARP/ICMP 応答を N 秒間実行 (既定 30) |
| `wifi resolve <host>` / `wifi web <host>` | DNS / DNS + HTTP GET |
| `wifi ntp [a.b.c.d]` | NTP 時刻クライアント |
| `wifi tftp <ip> <file>` / `wifi netboot <ip> <file>` | TFTP 取得 / 取得 + チェインロード |

接続の典型例:

```
xinu-pi4$ wifi probe          # ファームウェア DL (起動ごとに 1 回)
xinu-pi4$ wifi scan           # 周囲の AP を確認
xinu-pi4$ wifi up MyHome-5G mypassword
wifi up: connected; ARP/ICMP responder is now persistent.
xinu-pi4$ wifi ping 8.8.8.8
```

> 再起動後は自動再接続しません。起動のたびに `wifi probe` + `wifi up` を実行してください。`wifi netboot <ip> <file>` は WiFi 越しにカーネルを取得してチェインロードします (有線の chainload と同様、SD 交換なしの更新経路)。

### 7.6 複数 Xinu でのメッシュネットワーク (MANET ad-hoc)

複数の Pi 4 (および Pi 3) Xinu ボードを **アクセスポイント無し** で直接つなぎ、ピアツーピアのメッシュを組めます。802.11 の IBSS (ad-hoc) モード + オンデマンドの AODV ルーティングを使います (ドローン HIL デモの Pi 4 + Pi 3 ノードと同じ MANET スタック)。

各ノードは同じ ad-hoc セルに、別々のノード番号で参加します:

```
xinu-pi4$ wifi probe                       # 起動ごとに 1 回
xinu-pi4$ wifi adhoc <cell-ssid> [ch] [node]
```

- `<cell-ssid>` — ad-hoc セル名。**全ノードで同じ名前・同じチャンネル**にすること。
- `[ch]` — 802.11 チャンネル (既定 6)。
- `[node]` — 自分のノード番号 (既定 1)。静的 IP `10.0.0.<node>/24` を得ます。**ノードごとに別の番号**を与えること。

**例 — チャンネル 6 の 3 ノードセル:**

```
xinu-pi4$ wifi adhoc mesh1 6 1      # → 10.0.0.1
xinu-pi4$ wifi adhoc mesh1 6 2      # → 10.0.0.2
xinu-pi4$ wifi adhoc mesh1 6 3      # → 10.0.0.3
```

電波の届く範囲のノードは `10.0.0.x` で直接通信できます (`wifi ping 10.0.0.2`)。

**マルチホップ (AODV)。** 宛先が直接届かないとき、`wifi aodv <ip>` でオンデマンドに経路探索します:

```
xinu-pi4$ wifi aodv 10.0.0.3
[wifi] === AODV discover 10.0.0.3 ===
[wifi] aodv: RREQ id=1 for 10.0.0.3
[wifi] *** AODV route: 10.0.0.3 via 10.0.0.2, 2 hop ***
```

最小 AODV モジュール (M13, RFC 3561 コア) が RREQ をブロードキャスト (UDP port 654) し、範囲内の各ノードが中継して逆経路を記録、宛先が RREP を返して順経路を確立します。各ノードはピアのために自動で中継するので、直接届かない宛先も中間ノードが転送します (経路表は最大 16 エントリ)。

> IBSS/ad-hoc はインフラ (`wifi up`) モードとは独立です。AP に繋がっている場合は先に `wifi off`。ad-hoc も再起動後は復元しないので、各ノードで `wifi probe` + `wifi adhoc` を再実行してください。

### 7.7 HTTP 制御面とリモートシェル

`system/tcp_server.c` は有線インタフェース上で HTTP サーバ (既定ポート 80、`192.168.3.100`) を動かし、ボードをネットワーク越しに丸ごと操作できます:

| ルート | 内容 |
| --- | --- |
| `GET /shell?cmd=<cmd>` | 任意のシェルコマンドを実行し出力を返す (stdin なし) |
| `POST /cc` | **AIPL または C を投げて、その場でコンパイル・実行** (§7.8) |
| `POST /cc?stage=xlat` | AIPL を C に翻訳して**返すだけ**（実行しない。§7.8） |
| `GET /api/x/<path>?method=&args=` | `web_expose` で公開したアクタを叩く (§7.8) |
| `POST /compile` | C プログラムを JIT コンパイル&実行 (本文 = ソース) |
| `GET /chat` | オンデバイス LLM チャット |
| `GET /actor` `/send` `/gc` `/jitstats` | アクタ一覧 / 送信 / アクタプール GC / JIT カウンタ |
| `GET /usbdiag` `/pcie-init` `/pcie-enum` `/xhci-reset` | USB / PCIe bring-up 診断 |
| `GET /fault` `/mmio-read` `/mmio-write` `/mmio-sweep` | フォルトカウンタ + 生 MMIO peek/poke |
| `GET /api/aptab` `/api/aprun` `/api/mem` | アクタ表 / アクタ駆動の打ち切り / 空き領域 (§7.9) |
| `GET /wx` `/ticks` | W^X の実効性 / スタック番人・文脈交換 |
| `POST /reboot` | BCM2711 watchdog リセット |
| `POST /chainload` | 新カーネルをアップロードして起動 (SD 交換なし) |

実行時診断にこれら HTTP カウンタを使うのは、`uart_puts` が HTTP ワーカコンテキストでデッドロックするためです (PCIe/xHCI の bring-up ログはすべてブート時にシリアルへ出力)。

> **【注意】この HTTP 制御面には認証がありません。** `/mmio-write` は任意の
> ハードウェアレジスタへの書き込みを、`/chainload` は任意コードのロードを、
> そのまま誰にでも提供します。またカーネル全体が EL1 の単一アドレス空間で
> 動作し、ユーザ空間による保護はありません。信頼できないネットワークには
> 接続しないでください。

**SD 交換なしのカーネル更新 (chainload)。** `POST /chainload` は新カーネルをアップロードして RAM 上でそれにジャンプします。アップロードはヘルパスクリプトが包みます:

```sh
python3 tools/remote_chainload.py 192.168.3.100 compile/kernel8.img
```

不良イメージでも電源を入れ直すだけで戻ります (SD は無傷) ので開発ループが速くなります。アップロードには HTTP サーバが応答可能であることが必要です。

----------------------------------------------------------------------

### 7.8 AIPL を投げて動かす（`POST /cc`）

**AIPL のソースをそのまま投げられる。** 本文の最初の語（空白とコメントを読み飛ばして）
が `class` なら機内の前段 `abcl2c` が C に直し、そうでなければ C として扱う。
どちらも機内の C コンパイラが AArch64 の機械語に JIT して実行し、
**プログラムの出力がそのまま応答に返る**。

```sh
curl --data-binary @g1_hello.aipl http://192.168.3.100/cc
  hello, AIPL
  tick 1
  tick 2
  => 0
```

**正典ガイド 10 本がそのまま動く**（`select`・期限つきの `wait`・`ai_call`・
真偽値の `true`/`false` 表示まで一致）。詳細は AIPL ユーザーズガイド第39章。

段階を選べる。

| 呼び方 | 内容 |
| --- | --- |
| `POST /cc` | 翻訳して JIT する（既定） |
| `POST /cc?stage=xlat` | **翻訳した C を返すだけ。実行しない** |
| `POST /cc?resident=1` | 常駐ロード（`/api/x/` で後から到達できる） |

`?stage=xlat` は**板の健全性を測る窓**でもある。機内の翻訳結果と、
Mac 側で同じ `cc/abcl2c.c` をビルドした翻訳器の出力をバイト比較すればよい。
差が出たら板の状態が壊れている。**板を落とさずに判定できる唯一の方法。**

```sh
cc -DABCL2C_HOST_TEST -w -o /tmp/a2c cc/abcl2c.c
/tmp/a2c g1_hello.aipl > /tmp/host.c
curl --data-binary @g1_hello.aipl "http://192.168.3.100/cc?stage=xlat" > /tmp/board.c
diff /tmp/host.c /tmp/board.c
```

外部公開したアクタには、あとから HTTP で届く。

```sh
curl --data-binary @g5_web.aipl http://192.168.3.100/cc
curl 'http://192.168.3.100/api/x/echo?method=say&args=hi'
  => echo: hi
```

**注意点**

- **前のプログラムは置き換わる。** 公開ルート（`web_expose`）もプログラムと寿命を共にする。
- **連続で投げるときは 5 秒ほど空ける。**
- `wait(ms)` はそのまま効く（HTTP は専用のアプリ・ワーカーで処理されるため）。
  1 回の実行で眠れる合計は 10 秒まで。
- アクタは **Xinu の実プロセス**として走る。したがって `send` した相手が
  次の文より先に走ることがあり、出力順が回によって変わりうる
  （AIPL としてはどちらも正しい。`send` は順序を約束しない）。

**機内の小型言語モデル。** カーネルに Karpathy の `stories260K` を焼き込んであり、
AIPL から `ai_call(prompt)` で呼べる。機外へは一切出ない。1 回およそ **0.79 秒**で、
Pi 3・Pi 5・Mac の参照実装と**同じ生成文**になる。

#### AIPL のどこまでが動くか

正典 AIPL の\*\*出典\*\*（`src/lexer.mll` のキーワード、`src/typing_env.ml` の
組込み一覧、ユーザーズガイドの効果表）と突き合わせて、この装置で確かめてある。
投げているのは正典の型検査器 `tc` を通したものだけ。

> 以前ここには「25 機能中 25」と書いていたが、その 25 は\*\*こちらで切った区切り\*\*で、
> 出典に当たったものではなかった。数え直したところ `sender` / `timed_out` /
> `typeof` / `float` のフィールド宣言が落ちていた。数は出典から数えること。

| 機能 | 可否 | 備考 |
|---|---|---|
| `class` / `method` / `var` / フィールド | ○ | |
| `float x = 1.5;`（フィールド宣言） | ○ | 正典では `float` は予約語で `var` とは別 |
| `sender` | ○ | いま処理しているメッセージの送り主。`send sender.m();` |
| `timed_out(r)` | ○ | `result` の第三の観測子（`is_ok` / `value` と対） |
| `typeof(x)` | ○ | `int` / `float` / `bool` / `string` / `array` / `unit` |
| `neg(x)` | ○ | 正典では `int -> int` と `float -> float` の両方 |
| `new` / `init` / `send` / `send!` | ○ | `new` は `init` を必ず呼ぶ |
| `now` / `future` / `await` / 期限 | ○ | |
| `select` / `case` / `timeout` | ○ | 相手のメソッド先頭に横取りを差し込む |
| 真偽値・比較 | ○ | `true` / `false` と表示する |
| 文字列・`++` | ○ | |
| `wait(ms)` | ○ | 1 回の実行で眠れる合計は 10 秒まで |
| `web_listen` / `web_expose` | ○ | |
| `ai_call` | ○ | 機内モデル。機外へ出ない |
| `float`・浮動小数リテラル | ○ | 値のタグで区別する |
| 配列（`array_*`） | ○ | |
| 数学組込み（`sqrt` `exp` `log` `sin` …15 個） | ○ | libm と比べて誤差 1e-11 以下 |
| `call` / 自メソッド呼び出し | ○ | |
| `else` 無しの期限（`result<τ>`） | ○ | 失敗は `err` |
| `is_ok` / `value` | ○ | |
| `replyto` / `answer` / 引数型 `reply` | ○ | 下記の限界を見よ |
| `acquire` / `release` | ○ | 下記の限界を見よ |
| 型・効果・レベルの注釈 | 受理して捨てる | 検査は正典側（`tc`）で |
| `spawn("クラス名","名前")` | ○ | 公開表にその名前で載せる。`remote` から届く |
| `remote("host:port","actor")` | ○ | 下記。UDP/9010 で運ぶ |

**`remote(...)`。** 他ノードの公開アクター（`web_expose`）へ送る／呼ぶ。

```
var d = now remote("192.168.3.101", "echo").say(21) timeout 2000 else -1;
send remote("192.168.3.50", "echo").say(5);
```

運びかたは UDP/9010。電文は人が読める ASCII 一行で、3 台と Mac のホスト VM が
同じ言葉を話す（`tcpdump` や `nc` でそのまま覗ける）。

```
Q <reqid> <actor> <method> <arg...>
R <reqid> <値>
```


**呼ばれる側の登録。** `remote(...)` で呼ばれる側は、公開したアクターが板に
**常駐**していなければならない。Pi 4 では `POST /cc?resident=1` で投げること
（素の `POST /cc` は走らせて終わりなので、外から呼ぶ先が残らず `err` が返る）。
Pi 5 は素の `POST /cc` でも残る。

- 送るのはメソッド**番号ではなく名前**。番号は板ごとに違うので、相手の板が
  自分の `__method_id` で解く。
- 引数は 1 個まで。数字だけの文字列は相手側で整数として渡る
  （正典の `step(3)` が整数を取るため）。
- 返ってくるのは相手が描いた文字列を読み直した値。**型は運んでいない** ――
  正典の型検査器が両側の型を合わせている前提で成り立つ約束である。
- 期限切れ・宛先不明・メソッド不明はすべて失敗になる。`else` を書けばその値、
  書かなければ `err`（`result<τ>`）。
- ★ 宛先 MAC は ARP を引かない。最初の要求は「宛先 MAC = ブロードキャスト、
  宛先 IP = ユニキャスト」で出し、拾う判断は受信側で行う。応答は要求フレームの
  送り主へ返すので ARP が要らず、そこで学んだ MAC を覚えて二回目からは
  ユニキャストになる。実験室の数台にはこれで足り、ARP の状態機械を増やすより
  壊れにくい。


**`replyto` / `answer` の限界。** この装置では `reply(v)` は「戻り値」なので、
返信先を値として渡すために **`now` ごとに返信スロットを 1 つ取る**。
`replyto` を読んだ呼び出しだけがスロットを使い、読まなかった呼び出しは
**従来どおり素通しする**（既存プログラムの実行順序を変えないため）。
渡した先が `answer` しないまま 64 回ポンプを回しても埋まらなければ、
`cc: now: 返信先を渡した先が answer しませんでした` と出して打ち切る。
同時に飛ばせる返信先は 32 本、`now` の入れ子は 16 段まで。

**Pi 5 との違い。** Pi 4 のアクターは **Xinu の実プロセス**として走るので、
`replyto` を渡した先が走るのを待つときは、Pi 5 の協調ポンプではなく
`ap_run()`（全アクタが待ちに落ちるまで譲る）を回す。見えかたは同じである。

**`acquire` / `release` の限界。** 対と取得順序の検査は正典の型検査器が担う。
装置側が持つのは**実行時の見張りだけ**で、取っていない資源の `release` と、
実行の終わりに残った放し忘れを出力に出す。
**待つ錠にはしていない** ── `acquire` と `release` の間で他のアクターへ譲るのは
`wait` や `now` を挟んだときだけで、そこで待たせると板ごと止まりうるからである。


### 7.9 止まったときに読むもの

**板が生きているうちに読むこと。**

| ルート | 内容 |
| --- | --- |
| `GET /fault` | 直前の CPU 例外（ESR / FAR / ELR / SPSR / SP） |
| `GET /api/aptab` | アクタ表の生の姿（pid・プロセス状態・待ち行列・dead） |
| `GET /api/aprun` | `ap_run` の打ち切り、強制 kill、ワーカー立て直し |
| `GET /api/mem` | 空きの合計・最大ブロック・断片の数 |
| `GET /jitstats` | `spawn_fails`、メッセージの取りこぼし |
| `GET /ticks` | スタック番人（`stkbad`）、文脈交換の回数 |
| `GET /wx` | W^X が効いているか（自分で書いて試す） |
| `GET /reboot` | **ウォッチドッグで再起動**（電源に触らずに済む） |

`/reboot` は**板が生きているときだけ**効く。使い切る前に使うこと。
HTTP だけが止まって ping が通る場合は、アプリ・ワーカーが 25 秒で立て直される
（回数は `/api/aprun` に出る）。

----------------------------------------------------------------------

## 8. QEMU で試す

実機が無くてもシェルまでは触れる:

```sh
cd compile
make qemu          # インタラクティブ起動。Ctrl-A X で抜ける
make qemu-smoke    # 既定のスクリプトを流し込み qemu-smoke.log に保存
```

QEMU 上では:

- MIDR_EL1 が `0x414fd0b1` (QEMU が公開する Cortex-A76 の part 番号。
  実機 Pi 4 では Cortex-A72 が動く)
- `halt` は PSCI SYSTEM_OFF が `virt` でハンドルされクリーン終了
- ネットワーク・USB・SD・HDMI 出力は無し (ハード未モデル)

> **【既知の不具合】** 現在 `qemu` ターゲットはリンクに失敗します
> (`sd_last_int`、`xhci_msd_*` が未定義)。したがって `make qemu` /
> `make qemu-smoke` は動作しません。動作確認は実機、および次節の
> ホストテストで行ってください。

----------------------------------------------------------------------

## 8.5 ホスト側ユニットテスト

カーネルの一部はハードウェアに依存しない純粋なロジックなので、ビルド
マシン上でそのままコンパイルして実行できます。SD カードを往復させる必要が
なく高速です。

```sh
make -C test/host run       # 失敗すると非ゼロで終了
```

`test/host/memtest.c` は `mem/memory.c` を直接 `#include` し、`critical.h`
だけをホスト用スタブ (`test/host/critical.h`) で差し替えます。インクルード順の
指定により他のヘッダは実カーネルのものが解決されるので、テストと本番が同じ
定義を共有します。

現在カバーしているのは first-fit アロケータで、ヘッダ未満の分割余り、最小
サイズの解放、分割と合体の往復、20000 回のランダム churn (97 回ごとに
フリーリストの全不変条件を検査)、不正な解放 (NULL / サイズ 0 / ヒープ外 /
二重解放) の拒否です。`fs/fat32.c` (既に read/write コールバックを受け取る)、
`fs/vfs.c`、`cc/` が次の候補です。

----------------------------------------------------------------------

## 9. config.txt の主な設定

`sdcard/config_pi4.txt` (`make install_pi4` がカード上の `config.txt` へ
コピー) の重要項目:

| 行                              | 意味                                           |
|---------------------------------|------------------------------------------------|
| `arm_64bit=1`                   | AArch64 で起動                                 |
| `kernel=kernel8.img`            | Pi 4 用カーネル名                              |
| `kernel_address=0x80000`        | カーネルロードアドレス                         |
| `enable_uart=1`                 | UART を有効化                                  |
| `uart_2ndstage=1`               | bootloader 第 2 段の UART ログを出す           |
| `dtparam=uart0=on`              | UART0 を GPIO14/15 にルーティング              |
| `init_uart_clock=48000000`      | PL011 基準クロックを 48 MHz に固定 (115200 用) |
| `hdmi_force_hotplug=1`          | HDMI ホットプラグを強制 ON                     |
| `framebuffer_width=640`         | ファームウェア確保 FB の幅                     |
| `framebuffer_height=480`        | ファームウェア確保 FB の高さ                   |
| `framebuffer_depth=32`          | FB の bpp                                       |

KMS overlay (`dtoverlay=vc4-kms-v3d`) を残してあるが、Xinu 側は
ファームウェアが用意した simple-framebuffer を mailbox 経由で借りる
構成になっている。

----------------------------------------------------------------------

## 10. USB / 入力デバイスのサポート状況

Pi 4 のフルサイズ USB-A ポートは **VL805 (PCIe xHCI)** チップ経由で
ぶら下がっており、xHCI ドライバを自作して USB キーボード・マウスを
動かしている。

ポイントと制約:

- **USB-2.0 (黒) ポートに挿すこと。** USB-2.0 ハブ (Genesys、TT 動作) 経由
  なので LS の割り込み IN split transaction が届く。USB-3.0 (青) ポートは
  VIA USB3 ハブ側に入り、周期転送が届かず動かない (カーソルが動かない)。
- `shell` の `usb` コマンドは DWC2 USB HCD (USB-C OTG) の **診断ダンプ専用**。
- Pi 4 の USB-C ポートは **DWC2 OTG** だが、USPi (`extern/uspi/lib`)
  ドライバは現在無効化されている。
- VL805 のファームウェアは SD ブート時 PCIe RC がリセットのままになるため、
  `NOTIFY_XHCI_RESET` mailbox tag (`0x00030058`, devid `0x100000`) を
  boot 時に発行して xHCI を起こしている (詳細は `NEXT_SESSION_PI4.md`)。

シェルとのやりとりは:

| 機種     | 入力経路                       | 出力経路                              |
|----------|--------------------------------|---------------------------------------|
| Pi 4     | USB-A キーボード / UART0 シリアル | UART0 シリアル + HDMI FB (シェル窓)   |
| QEMU     | qemu のシリアル stdio          | qemu のシリアル stdio                  |

シリアル経由で操作する場合は USB-シリアル変換ケーブル + ホスト PC の
`screen` 等が必要。

----------------------------------------------------------------------

## 11. トラブルシューティング

| 症状                                | 確認ポイント                                                          |
|-------------------------------------|-----------------------------------------------------------------------|
| シリアルに何も出ない                | (1) `screen` のデバイス名と速度、(2) TX/RX の入れ違い、(3) GND 接続  |
| バナーは出るがすぐ止まる            | `kernel8.img` の md5 が古い可能性。再ビルド + 再コピー               |
| プロンプトでキー入力が echo されない| ターミナルが local echo OFF。 `screen` は OFF が正しい                |
| `peek` で 0xFFFFFFFF が返る         | そのアドレス帯が未マップ。 MMU はまだ flat ID なので有効領域に注意   |
| USB キーボード/マウスが効かない     | USB-**2.0 (黒)** ポートに挿しているか確認 (§10)                       |
| ping が通らない (Pi 4)              | (1) `sudo arp -s ...` 済か、(2) ケーブル接続、(3) `rxstat` で RX 増分 |
| QEMU で `make qemu` が失敗          | `qemu-system-aarch64` のバージョン確認 (11 以降推奨)                  |
| `make install` が permission error  | 内部で `sudo cp` するため、パスワード入力要求が来る                   |
| 起動後すぐ reboot ループ            | `config.txt` の `kernel=` 行と実ファイル名が一致しているか            |

ハマったときの定番動作:

1. `cd compile && make clean && make pi4`
2. `make install_pi4 SDCARD=/Volumes`
3. `diskutil eject /Volumes/bootfs`
4. SD カード抜き挿し → Pi の電源再投入

----------------------------------------------------------------------

## 12. ディレクトリ構成 (利用者目線)

普段触る場所だけ抜粋:

```
xinu-rpi4/
├── compile/                # ビルドはここで `make`
│   ├── Makefile
│   ├── kernel8.img         # Pi 4 用 (make pi4 で生成)
│   └── kernel_virt.img     # QEMU 用 (make qemu で生成)
├── sdcard/
│   └── config_pi4.txt      # Pi 4 用 (install_pi4 がコピー)
├── test/host/              # ホスト側ユニットテスト (make -C test/host run)
├── docs/
│   ├── OS_ASSESSMENT_JA.md # OS としての改良余地アセスメント
│   └── SMP_REPORT_JA.md    # SMP + D-cache レポート
├── README.md               # 開発者向けロードマップ + 内部実装
├── USERS_MANUAL_JA.md      # 本ファイル
└── NEXT_SESSION_PI4.md     # 進行中作業のハンドオフメモ
```

ソースコードを読む場合は `README.md` の Layout 節を参照。

----------------------------------------------------------------------

## 13. よくある操作レシピ集

### ビルドから Pi 4 で起動するまで一気通貫

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make clean
make pi4
make install_pi4 SDCARD=/Volumes
diskutil eject /Volumes/bootfs
# SD カードを Pi 4 に差し戻して電源を入れる
screen /dev/tty.usbserial-XXXX 115200
```

### Pi 4 で ping を通す

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make pi4
make install_pi4 SDCARD=/Volumes
diskutil eject /Volumes/bootfs
# Pi 4 を起動

# Mac 側で:
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
```

### コマンドを手早く確かめる (QEMU)

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make qemu
# プロンプトが出たら:
xinu-pi4$ procdemo 5
xinu-pi4$ pingpong 3
xinu-pi4$ halt
# Ctrl-A X で QEMU から脱出
```

----------------------------------------------------------------------

## 14. 参考リンク

- 上流 Xinu (32-bit): https://github.com/yaskodama/xinu-rpi
- AArch64 boot stub の元: https://github.com/radlyeel/leex
- シェル centry パターンの元: https://github.com/davidxyz/xinuPi
- 開発ロードマップ (Round 1 計画書):
  abclcp-project リポジトリの `aice-pi-evolution/experiments/` 配下

----------------------------------------------------------------------

## 15. ライセンス

上流 Xinu と leex の BSD 系ライセンスを継承。詳細は `LICENSE` を参照
(整備中)。
