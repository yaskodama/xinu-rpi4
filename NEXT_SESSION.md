# NEXT_SESSION — Pi 4 xinu-rpi5 status (HEAD `6fcced5` + WIP)

## このセッションで達成したこと
- **NET-C3: TX 1 frame 動作** (broadcast ARP、CONS=1 sent OK)
- **NET-D: RX ring + frame 受信動作** (Pi 4 が LAN broadcast を受信して shell window に dst/src/type を表示)
- Pi 4 GENET の **descriptor format / register layout の正解** を確定:
  - per-ring stride 0x40、ring 16 = `0x4400` ... wait, no! Final correct values:
    - TX_RING_BASE = GENET_TDMA_REG_OFF + 16*0x40 = `0x5000`
    - TX_DMA_TOP   = GENET_TDMA_REG_OFF + 17*0x40 = `0x5040`
    - GENET_TDMA_REG_OFF = `0x4000 + 256*12 = 0x4C00`
  - **TX descriptors live in MMIO** at GENET_BASE + 0x4000 + idx*12 (NOT in DRAM)
  - Word 0 layout: bits 31:16 = length, bits 15:0 = status
    - SOP=0x2000, EOP=0x4000, OWN=0x8000 (RX), WRAP=0x1000, CRC=0x40, QTAG@bit 7
  - DMA_CTRL = `(1 << (DEFAULT_Q + 1)) | 1` = `0x20001` (bit 0 = DMA_EN, bit 17 = ring 16 data-path)

## NET-E (xinu-raz network/ port) — 着手したが課題判明

### 試したこと
- `/Users/kodamay/projects/xinu-raz/xinu/network/{arp,ipv4,icmp,netaddr,net}` (41 .c files, 7297 行) を `xinu-rpi5/network/` にコピー済み
- xinu-raz の 83 個の include を `xinu-rpi5/extern/xinu-raz-include/` に隔離 (xinu-rpi5 既存ヘッダーと混ぜない)

### 残る壁
xinu-raz の network/ コードは Xinu kernel 全機能に依存:
- **`semaphore.h`** — semcreate / wait / signal / semreset
- **`mailbox.h`** — mbox-based inter-process messaging
- **`thread.h` / `proc.h`** — preemptive scheduler with ready/wait/sleep
- **`bufpool.h`** — pool allocator for ethpkt buffers
- **`device.h`** — Xinu device abstraction (open/close/read/write/control)
- **`interrupt.h`** — Xinu IRQ control (currently we have raw connect_interrupt)

xinu-rpi5 は cooperative scheduler のみ。これらを実装 or stub する必要あり。

### 次セッションの計画

#### Step 1: Xinu kernel 互換 stub 層 (1 セッション)
- `sem`: 単純なカウンタ + busy-wait (cooperative scheduler 想定)
- `mbox`: シングルスレッドなので queue + immediate dispatch
- `ready/wait`: NULLPROC ベースでスキップ
- `bufpool`: 固定 array allocator
- これらを `system/xinu_compat.c` に集約

#### Step 2: ARP 単独ビルド (1 セッション)
- `network/arp/*.c` だけ Makefile に追加
- ビルド通るまで `-I../extern/xinu-raz-include` + 個別 fix
- `arpResolve` を Pi 4 GENET 上で動作させる

#### Step 3: IPv4 + ICMP (1 セッション)
- 同じくビルド + 接続
- 目標: Mac から Pi 4 (固定 IP) に `ping` 通る

#### Step 4: DHCP + TCP + telnet (2-3 セッション)
- DHCP で IP 取得
- TCP + telnet daemon で shell remote login

### 別の選択肢: Plan A (今は除外)
- 最小 ARP responder を 200-300 行で自作
- ping 通るまで 1 セッションで到達可能
- ただし TCP/IP stack 全体を後で再実装

User の判断は **B 継続** だったので Step 1-4 で行く。

## このセッションの commit chain
- `1974eec` — shell window + soft kbd + cursor + virtual desktop + S1 + USB-M0 + xHCI scaffold
- `4fdea3f` — NET-A/B/C1 (GENET probe, UMAC, BCM54213PE link UP)
- `6a204f8` — NET-C2/C3 stub (TDMA register layout, descriptor format research)
- `e99008d` — NET-C3 fixed descriptor format (length<<16 | status low16)
- `62f20f4` — NEXT_SESSION.md initial handoff
- `1c15b21` — **NET-C3 TX works** (CONS=1 sent OK)
- `6fcced5` — **NET-D RX works** (broadcast frames received and logged)

## ビルド/焼き
```
cd /Users/kodamay/projects/xinu-rpi5/compile
make pi4                          # → kernel8.img (~42 KB)
diskutil mount /dev/disk4s1
cp kernel8.img /Volumes/bootfs/
sync
diskutil eject /Volumes/bootfs
```
