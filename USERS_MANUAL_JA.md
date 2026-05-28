# Xinu xinu-rpi4 ユーザーズマニュアル (日本語版)

Raspberry Pi 5 (BCM2712 / Cortex-A76, AArch64) 向けの Embedded Xinu 移植版。
同じソースツリーから Pi 4 (BCM2711) と QEMU `virt` 機にもクロスビルドできる。
本マニュアルは「動かす側」向け — リポジトリの開発ロードマップは `README.md`
を参照のこと。

----------------------------------------------------------------------

## 1. 対象ターゲットとビルド成果物

| ターゲット | ボード / 機種            | 出力イメージ        | UART0 base     |
|------------|--------------------------|---------------------|----------------|
| `pi5`      | Raspberry Pi 5 (BCM2712) | `kernel_2712.img`   | `0x107D001000` |
| `pi4`      | Raspberry Pi 4 (BCM2711) | `kernel8.img`       | `0xFE201000`   |
| `qemu`     | `qemu-system-aarch64 -M virt -cpu cortex-a76` | `kernel_virt.img` | `0x09000000` |

全ターゲットとも load address は `0x80000` (Pi 5/4) / `0x40080000` (QEMU)。
3 つのイメージは同一ソースから `-D<TARGET>` と異なるリンカスクリプトで作り
分けている。

### 1.1 機能対応マトリクス (重要)

「全ターゲットで動く」訳ではない。 **Pi 4 でしか動かない機能** が複数あるので
要注意:

| サブシステム            | Pi 5 | Pi 4 | QEMU virt | 備考                                         |
|-------------------------|:----:|:----:|:---------:|----------------------------------------------|
| PL011 UART0 シェル      |  ✅  |  ✅  |    ✅     | 全機種で動く土台                              |
| 協調的スケジューラ (S0) |  ✅  |  ✅  |    ✅     | `procdemo` / `pingpong`                       |
| プリエンプティブ (S1)   |  ⏳  |  ✅  |    ⏳     | Pi 4 のみ 100 Hz tick 稼働 (`ticks`)         |
| HDMI フレームバッファ   |  ⏳  |  ✅  |    —      | Pi 5 は KMS/firmware FB 受け渡し未確立        |
| ウィンドウマネージャ    |  ⏳  |  ✅  |    —      | 1280×960 仮想デスクトップは Pi 4 のみ実用     |
| SD カード + FAT32       |  ⏳  |  ✅  |    —      | EMMC ドライバが Pi 4 (BCM2711) 向け         |
| Ethernet (GENET)        |  —   |  ✅  |    —      | NET-E 完了。 Pi 5 は GENET ではなく RP1 経由 |
| **USB (キーボード/HID)**|  —   |  ❌  |    —      | **未サポート — 後述 §11 参照**                |
| DHCP / TCP              |  ❌  |  ❌  |    —      | コードは在るが dispatch OFF (§7.4)            |

> ⚠ ソースツリーには `device/genet/`, `device/sd/`, `device/usb/xhci/` 等が
> 同居しているが、これらは **Pi 4 (BCM2711) のレジスタアドレスで初期化** される。
> Pi 5 でビルドすると `-DGENET_BASE` 等が undef になりリンクから外れる作りで、
> 「Pi 5 でも勝手にネットが動く」ことは無い。

### 1.2 起動シーケンス (特殊な部分)

Pi 5 のファームウェアは少し変わった要件を出すので、`loader/boot.S` は
通常の bare-metal stub よりひと工夫が要る:

1. **Linux ARM64 Image ヘッダが必須**
   - Pi 5 の EEPROM bootloader はファイル先頭から 0x38 オフセットに
     `"ARM\x64"` マジックが無いとカーネルへジャンプしない
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

Pi 5 は config.txt が `arm_64bit=1` だけのとき **EL1 で開始** する。
EL2→EL1 のドロップは自分で実装していない (Pi 4/QEMU も同じ前提)。
config.txt で `armstub` を指定したり、`kernel_old=1` 等を入れると
状況が変わるので注意。

----------------------------------------------------------------------

## 2. 用意するもの

### 2.1 ハードウェア (実機の場合)

- Raspberry Pi 5 または Pi 4
- microSD カード (FAT32 で `bootfs` パーティションがあるもの)
  - 一番簡単なのは Raspberry Pi OS をいったん焼き、後で `kernel_*.img`
    と `config.txt` を上書きする方式
- USB-シリアル変換ケーブル (3.3V、115200 8N1)
  - ヘッダピン 8 (TXD → GPIO14) / 10 (RXD → GPIO15) / 6 (GND) に接続
- Pi 4 のネットワーク機能を使う場合: イーサネットケーブル

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

make pi5            # → compile/kernel_2712.img  (Raspberry Pi 5 実機)
make pi4            # → compile/kernel8.img      (Raspberry Pi 4 実機)
make qemu           # → compile/kernel_virt.img  (QEMU virt)
make                # = make all = pi4 + pi5 + qemu
make clean          # オブジェクト・全 .img を削除
```

各ターゲットのオブジェクトは別ツリーに分離される
(`obj/pi5/`, `obj/pi4/`, `obj/qemu/`) ので、続けて別ターゲットを
ビルドしても再コンパイルは差分のみ。

### 3.2 ツールチェインの自動検出

Makefile は次の順で AArch64 GCC を探す:

1. `$(GCCPATH)/bin/aarch64-elf-gcc` (Homebrew `aarch64-elf-gcc`)
2. `$(GCCPATH)/bin/aarch64-none-elf-gcc` (ARM 公式 `gcc-arm-embedded`)

`GCCPATH` の既定値は `/opt/homebrew`。 別の場所にインストールしたなら:

```sh
make pi5 GCCPATH=$HOME/aarch64/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
```

プレフィックスも上書き可能:

```sh
make pi5 CROSS=aarch64-linux-gnu-
```

### 3.3 主な Make 変数

| 変数         | 既定値                  | 用途                              |
|--------------|-------------------------|-----------------------------------|
| `GCCPATH`    | `/opt/homebrew`         | クロスコンパイラのインストール先  |
| `CROSS`      | (自動検出)              | プレフィックス (`aarch64-elf-` 等)|
| `SDCARD`     | `/Volumes`              | `make install_*` のマウント親     |
| `DEST`       | `$(SDCARD)/bootfs`      | コピー先パーティション            |

### 3.4 ビルド時の主な定義 (CFLAGS で渡る)

| マクロ          | Pi 5             | Pi 4             | QEMU virt    |
|-----------------|------------------|------------------|--------------|
| `-mcpu`         | `cortex-a76`     | `cortex-a72`     | `cortex-a76` |
| `UART0_BASE`    | `0x107D001000`   | `0xFE201000`     | `0x09000000` |
| `MBOX_BASE`     | `0x107C00B880`   | `0xFE00B880`     | (未定義)     |
| `SD_BASE`       | —                | `0xFE340000`     | —            |
| `USB_BASE`      | —                | `0xFE980000`     | —            |
| `GIC_BASE`      | —                | `0xFF840000`     | —            |
| `PCIE_BASE`     | —                | `0xFD500000`     | —            |
| `GENET_BASE`    | —                | `0xFD580000`     | —            |
| `HEAP_END`      | `0x40000000` (1G)| `0x40000000` (1G)| `0x50000000` |
| `SKIP_MBOX`     | —                | —                | 定義 (簡略化)|
| `KERNEL_NAME`   | `"kernel_2712.img"` | `"kernel8.img"` | `"kernel_virt.img"` |
| `BOARD_NAME`    | `"Pi5"`          | `"Pi4"`          | `"virt"`     |
| `SOC_NAME`      | `"BCM2712"`      | `"BCM2711"`      | `"QEMU"`     |

共通 CFLAGS:
`-Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mgeneral-regs-only`

`-mgeneral-regs-only` 指定により FPU/NEON は使えない (浮動小数は手書き
ソフトエミュレーション側へ寄せる前提)。

### 3.5 何がリンクされているか

`Makefile` の `COMPONENTS` リスト:

```
loader mem system shell fs
device/uart device/mbox device/video device/sd device/usb
device/usb/xhci device/gic device/timer device/genet
network/arp network/net network/netaddr network/ipv4 network/icmp
```

すべて `*.c` / `*.S` を `$(wildcard ...)` で吸い上げる。
ただし Pi 5 ビルドでは `GENET_BASE` 等が未定義のため、対応コードは
コンパイルエラーまたは no-op になる (Pi 4 専用周辺は §1.1 表の通り)。

### 3.6 リンカスクリプト

| スクリプト         | 対象      | エントリ・ロードアドレス  |
|--------------------|-----------|---------------------------|
| `compile/link.ld`  | Pi 4/Pi 5 | `0x80000`                 |
| `compile/link_virt.ld` | QEMU virt | `0x40080000`           |

どちらも `.text.boot` を先頭に置き、`__bss_start` / `__bss_size`
(8-byte 単位) をエクスポートする leex 風の構成。

----------------------------------------------------------------------

## 4. SD カードへの書き込み

### 4.1 Pi 5

```sh
cd compile
make install_pi5 SDCARD=/Volumes
# → /Volumes/bootfs に kernel_2712.img と sdcard/config.txt をコピー
```

`make install` は `install_pi5` のエイリアス。

### 4.2 Pi 4

```sh
cd compile
make install_pi4 SDCARD=/Volumes
# → /Volumes/bootfs に kernel8.img と sdcard/config_pi4.txt をコピー
```

### 4.3 手動で焼く場合 (例: 別の SD リーダー)

```sh
diskutil mount /dev/disk4s1                # → /Volumes/bootfs
cp compile/kernel8.img /Volumes/bootfs/
cp sdcard/config_pi4.txt /Volumes/bootfs/config.txt
sync
diskutil eject /Volumes/bootfs
```

> ⚠ `kernel_*.img` の md5 が SD カード上のものと一致するか必ず確認すること。
> 古いイメージで起動して混乱するのは「あるある」。

### 4.4 SD カードに必要なもの

`kernel_*.img` と `config.txt` だけでは起動しない。以下も同じ FAT32
パーティションに置く必要がある:

- Raspberry Pi OS の `bootcode.bin`, `start4.elf`, `fixup4.dat` ほか
  ファームウェア blob 一式
- 通常は Raspberry Pi OS をフラッシュした後、`kernel_*.img` と
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
  Xinu Pi5 hello (AArch64, BCM2712, kernel_2712.img)
  PL011 UART0 @ 0x107D001000, 115200 8N1
  bootstrap: leex-style stub + xinu-rpi4 main
================================================

Round 1 phase B/U done — entering interactive shell.
type `help` for the command list.
xinu-pi4$ _
```

Pi 4 でビルドした場合は `xinu-pi4$` プロンプト。

----------------------------------------------------------------------

## 6. シェルコマンドリファレンス

`help` または `?` で常に最新の一覧が見られる。以下は標準コマンド:

| コマンド           | 用途                                                       |
|--------------------|------------------------------------------------------------|
| `help` / `?`       | 登録コマンド一覧を表示                                     |
| `echo <words…>`    | 引数をそのままエコーバック                                 |
| `hello`            | 動作確認用の挨拶                                           |
| `mem`              | `__bss_start` / `__bss_end` / `_end` を表示 (link.ld 由来) |
| `peek <hex_addr>`  | 32-bit MMIO ワードを読む (例: `peek 0x107d001018`)         |
| `uptime`           | 生の `CNTPCT_EL0` (汎用タイマカウンタ) を表示              |
| `ticks`            | 100 Hz タイマ tick の累積数 (S1 GIC+Timer 後)              |
| `ps`               | コア / EL ステータス簡易表示 (スケジューラ前のスナップ)    |
| `halt`             | DAIF マスク + PSCI `SYSTEM_OFF` (QEMU `virt` ならクリーン終了) |
| `reboot`           | スタブ — 電源再投入待ちでスピン                            |
| `pingpong [N]`     | AIPL 風 2 アクター協調 PingPong (N=1..50, 既定 5)          |
| `procdemo [N]`     | 実 ctxsw による 2 プロセスデモ (N=1..30, 既定 5)           |
| `usb`              | DWC2 USB HCD 診断 (Pi 4 のみ)                              |
| `rxstat`           | RX リングを drain してパケット/バイトカウンタを表示        |
| `pan <dx> <dy>`    | 仮想 1280×960 デスクトップのビューポートをスクロール       |
| `view`             | ビューポート / デスクトップサイズを表示                    |
| `autopan [on|off]` | デモ用オートパンの切替                                     |

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

----------------------------------------------------------------------

## 7. ネットワーク機能 (Pi 4 のみ)

`xinu-rpi4` の現在のネットワークドライバは Pi 4 GENET (BCM2711) のみ動作。
Pi 5 は RP1 経由で構造が違うため別作業中。

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

`system/dhcp_client.c` (DHCP クライアント) と `system/tcp_server.c`
(単一接続 TCP listener) のコードはツリーに含まれているが、現在は
`rx_tick` からの **dispatch を OFF** にしている:

- **DHCP**: `dhcp_send_discover()` で wire には正しく出るが、家庭用 router
  の WiFi↔LAN bridge が DHCP broadcast を片方向しか通さないケースが多く、
  OFFER が戻らない。連続 broadcast TX で GENET TX ring が劣化し ICMP
  reply が止まる症状もあり、boot-time DISCOVER と 5 秒周期リトライは外した。
- **TCP**: SYN+ACK / echo / FIN まで実装済み、port 23 SYN は届くことを
  確認済みだが、`tcp_handle_packet()` を `rx_tick` から呼ぶだけで以降の
  ICMP echo reply が止まる現象が出る (詳細は `NEXT_SESSION.md`)。

つまり「動くコードはあるが既定では呼ばれない」状態。再有効化は今後の
スプリント (NET-G bisect) で対応予定。

----------------------------------------------------------------------

## 8. QEMU で試す

実機が無くてもシェルまでは触れる:

```sh
cd compile
make qemu          # インタラクティブ起動。Ctrl-A X で抜ける
make qemu-smoke    # 既定のスクリプトを流し込み qemu-smoke.log に保存
```

QEMU 上では:

- MIDR_EL1 が `0x414fd0b1` (Cortex-A76 の公式 part 番号)
- `halt` は PSCI SYSTEM_OFF が `virt` でハンドルされクリーン終了
- ネットワーク・USB・SD・HDMI 出力は無し (ハード未モデル)

----------------------------------------------------------------------

## 9. config.txt の主な設定

`sdcard/config.txt` (Pi 5) と `sdcard/config_pi4.txt` (Pi 4) に共通の
重要項目:

| 行                              | 意味                                           |
|---------------------------------|------------------------------------------------|
| `arm_64bit=1`                   | AArch64 で起動                                 |
| `kernel=kernel_2712.img`        | Pi 5 用カーネル名 (Pi 4 は `kernel8.img`)      |
| `kernel_address=0x80000`        | カーネルロードアドレス                         |
| `enable_uart=1`                 | UART を有効化                                  |
| `uart_2ndstage=1`               | bootloader 第 2 段の UART ログを出す           |
| `dtparam=uart0=on`              | UART0 を GPIO14/15 にルーティング              |
| `hdmi_force_hotplug=1`          | HDMI ホットプラグを強制 ON                     |
| `framebuffer_width=640`         | ファームウェア確保 FB の幅                     |
| `framebuffer_height=480`        | ファームウェア確保 FB の高さ                   |
| `framebuffer_depth=32`          | FB の bpp                                       |

KMS overlay (`dtoverlay=vc4-kms-v3d`) を残してあるが、Xinu 側は
ファームウェアが用意した simple-framebuffer を mailbox 経由で借りる
構成になっている。

----------------------------------------------------------------------

## 10. USB / 入力デバイスのサポート状況

**現状: USB 経由のキーボード・マウスは未サポート。** 入力は全て UART
シリアル経由となる。

理由と中身:

- Pi 4 のフルサイズ USB-A ポート (×4) は **VL805 (PCIe xHCI)** チップ
  経由でぶら下がっており、xHCI ドライバを自作する必要がある
  - リポ内 `device/usb/xhci/` はスケルトンのみ — enumeration / port reset
    / HID transfer は未実装
  - `shell` の `usb` コマンドは DWC2 USB HCD の **診断ダンプ専用**
    (デバイス制御は不可)
- Pi 4 の USB-C ポートは **DWC2 OTG** だが、USPi (`extern/uspi/lib`)
  ドライバは現在無効化されている
- Pi 5 は **RP1 I/O hub** 経由で USB が出ているため、Pi 4 とは全く別の
  ドライバが要る (こちらも未着手)

つまり、シェルとのやりとりは:

| 機種     | 入力経路              | 出力経路                              |
|----------|------------------------|---------------------------------------|
| Pi 5     | UART0 シリアルのみ     | UART0 シリアルのみ                    |
| Pi 4     | UART0 シリアルのみ     | UART0 シリアル + HDMI FB (シェル窓)   |
| QEMU     | qemu のシリアル stdio  | qemu のシリアル stdio                  |

USB キーボードからの直接入力ができないため、必ず USB-シリアル変換
ケーブル + ホスト PC の `screen` 等が必須。

----------------------------------------------------------------------

## 11. トラブルシューティング

| 症状                                | 確認ポイント                                                          |
|-------------------------------------|-----------------------------------------------------------------------|
| シリアルに何も出ない                | (1) `screen` のデバイス名と速度、(2) TX/RX の入れ違い、(3) GND 接続  |
| バナーは出るがすぐ止まる            | `kernel_*.img` の md5 が古い可能性。再ビルド + 再コピー               |
| プロンプトでキー入力が echo されない| ターミナルが local echo OFF。 `screen` は OFF が正しい                |
| `peek` で 0xFFFFFFFF が返る         | そのアドレス帯が未マップ。 MMU はまだ flat ID なので有効領域に注意   |
| Pi 5 で HDMI 真っ黒                 | これは現状の仕様。framebuffer console は Pi 4 のみ実用段階            |
| ping が通らない (Pi 4)              | (1) `sudo arp -s ...` 済か、(2) ケーブル接続、(3) `rxstat` で RX 増分 |
| QEMU で `make qemu` が失敗          | `qemu-system-aarch64` のバージョン確認 (11 以降推奨)                  |
| `make install` が permission error  | 内部で `sudo cp` するため、パスワード入力要求が来る                   |
| 起動後すぐ reboot ループ            | `config.txt` の `kernel=` 行と実ファイル名が一致しているか            |

ハマったときの定番動作:

1. `cd compile && make clean && make pi5` (or `pi4`)
2. `make install_pi5 SDCARD=/Volumes`
3. `diskutil eject /Volumes/bootfs`
4. SD カード抜き挿し → Pi の電源再投入

----------------------------------------------------------------------

## 12. ディレクトリ構成 (利用者目線)

普段触る場所だけ抜粋:

```
xinu-rpi4/
├── compile/                # ビルドはここで `make`
│   ├── Makefile
│   ├── kernel_2712.img     # Pi 5 用 (make pi5 で生成)
│   ├── kernel8.img         # Pi 4 用 (make pi4 で生成)
│   └── kernel_virt.img     # QEMU 用 (make qemu で生成)
├── sdcard/
│   ├── config.txt          # Pi 5 用 (install_pi5 がコピー)
│   └── config_pi4.txt      # Pi 4 用 (install_pi4 がコピー)
├── README.md               # 開発者向けロードマップ + 内部実装
├── USERS_MANUAL_JA.md      # 本ファイル
└── NEXT_SESSION.md         # 進行中作業のハンドオフメモ
```

ソースコードを読む場合は `README.md` の Layout 節を参照。

----------------------------------------------------------------------

## 13. よくある操作レシピ集

### ビルドから Pi 5 で起動するまで一気通貫

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make clean
make pi5
make install_pi5 SDCARD=/Volumes
diskutil eject /Volumes/bootfs
# SD カードを Pi 5 に差し戻して電源を入れる
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
  `aice-pi-evolution/experiments/2026-05-22_xinu_rpi5/AIPL_XinuRPi5_Round1.aice`

----------------------------------------------------------------------

## 15. ライセンス

上流 Xinu と leex の BSD 系ライセンスを継承。詳細は `LICENSE` を参照
(整備中)。
