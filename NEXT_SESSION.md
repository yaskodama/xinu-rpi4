# NEXT_SESSION — Pi 4 xinu-rpi5 (HEAD after Step 3 partial)

## このセッションでの達成

### Step 1 ✅: Xinu kernel compat layer
`system/xinu_compat.c` で xinu-raz network/ コードが要求する Xinu
kernel API をすべて stub:
- sem: semcreate / wait / signal / signaln / semcount / semfree
- mailbox: mailboxAlloc / Send / Receive / Count / Free / Init
- bufpool: bfpalloc / bufget / buffree / bfpfree (kmalloc 背景)
- thread: create / gettid / ready / kill / sleep / yield / send / recvtime
- IRQ: disable / restore (irqmask)
- kprintf: %d %u %x %s %c %p with `l` length

### Step 2 ✅: network/{arp,net,netaddr,ipv4,icmp} ビルド
xinu-raz の 41 ファイル / 7300 行が xinu_compat 上で link。
追加対応:
- `typedef char bool` を modern C 用にコメントアウト
- arm-qemu の `interrupt.h` を extern にコピー (irqmask 型)
- libc shims (bzero/memcmp/sprintf/sscanf), clock (clktime/clkcount),
  device (read/write/control/devtab), routing (rtInit/Lookup/...),
  upper proto stubs (tcpRecv/udpRecv/rawRecv/snoopCapture) を
  xinu_compat に追加

### Step 3 ⚠️ partial: ARP+ICMP responder
`system/net_responder.c` に最小 ARP responder + ICMP echo responder
を実装。`net_responder_handle(pkt, len)` を wm tick から呼ぶ。
- 受信した ARP request に対して reply を生成 → `genet_tx_frame()`
- 受信した ICMP echo に対して reply を生成 → `genet_tx_frame()`
- 固定 IP 192.168.3.100、Pi 4 MAC d8:3a:dd:a7:fd:bf
- gratuitous ARP × 1 を boot 時に送出 (Mac へ ARP cache 通知)

### NET-D の余波
boot 中の trace prints も入っている。最後の行に
`net: PHY BMSR = 0x????????  link=UP` が出る。

## 本セッション最大の未解決バグ

**`genet_tx_frame` の 2 回目以降の呼び出しで kernel が hang する。**

症状:
- 起動時の `genet_send_one_arp` (= NET-C3 で動作確認済み) は OK
- その後 `genet_tx_frame()` 経由の 2 回目 TX (gratuitous ARP × 1
  や ARP reply、ICMP reply 等) で完全 hang
- `net: gratuitous ARP done` が出ない → genet_tx_frame の中で
  polling loop に入ったまま戻らない

仮説:
- TX descriptor 1 以降への書き込みが期待通り動作していない
- CONS_INDEX が advance しないが、50 ms timeout に到達するはずなのに
  to 達せずに hang してる = MMIO read 自体が刺さってる?
- PROD_INDEX セマンティクスを誤解している可能性
  - 現在: `PROD_INDEX = readl + 1` を書く
  - U-Boot の bcmgenet_gmac_eth_send も同じ動き
- 内部 TX engine が「ring を一回 walk して停止」、再起動が必要かも
  - DMA_CTRL の per-ring enable bit を再 toggle?

次セッションで debug:
1. `genet_tx_frame` に trace を追加して、どの MMIO アクセスで止まるか
   ピンポイント
2. U-Boot drivers/net/bcmgenet.c::bcmgenet_gmac_eth_send をもう一度
   精読、私の実装と diff を取る
3. TDMA_STATUS, DMA_CTRL, ring registers を 2 回目 TX 前後で dump

## Step 3 完成までの作業

1. ⬜ `genet_tx_frame` 2 回目以降の hang を fix
2. ⬜ Pi 4 が gratuitous ARP を実際に Mac WiFi に届かせる
   (router の LAN→WiFi bridge 確認)
3. ⬜ Mac から ARP request を受信 → reply 送信
4. ⬜ Mac から `ping 192.168.3.100` 通る

router の WiFi-LAN bridge の方向性問題は別軸 (network operator
configuration)。最悪 Mac に USB-Ethernet adapter で物理回避可。

## ネットワーク経路の状況

- Mac WiFi: 192.168.3.202/24 (en0)
- Pi 4 LAN: 192.168.3.100/24 (固定、xinu-rpi5 設定)
- Mac → Pi 4 ARP: 届かない (Mac arp -a で incomplete)
- Pi 4 → Mac broadcast: 未確認 (TX 2 回目以降が動かないため)
- 他ホスト (192.168.3.198/200/201) は Mac から見える + Pi 4 も
  broadcast 受信可能 → 同一サブネットだが broadcast 経路の方向性に
  問題ある可能性

## このセッションの commit chain

- `1974eec` shell window + soft kbd + cursor + virtual desktop +
  S1 IRQ + USB-M0 + xHCI scaffold
- `4fdea3f` NET-A/B/C1 GENET probe + PHY Link UP
- `6a204f8` NET-C2/C3 TDMA layout research
- `e99008d` NET-C3 desc format fix
- `62f20f4` NEXT_SESSION initial
- `1c15b21` **NET-C3 TX works** (CONS=1)
- `6fcced5` **NET-D RX works** (frames received)
- (NET-E groundwork commit)
- `c3ad476` Step 1 Xinu kernel compat layer
- `30976e1` Step 2 network/{arp,net,netaddr,ipv4,icmp} build
- (this commit) Step 3 partial — ARP+ICMP responder + gratuitous ARP,
  TX 2nd-call hang outstanding

## ビルド / 焼き

```
cd /Users/kodamay/projects/xinu-rpi5/compile
make pi4
diskutil mount /dev/disk4s1
cp kernel8.img /Volumes/bootfs/
sync
diskutil eject /Volumes/bootfs
```
