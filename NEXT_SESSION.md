# NEXT_SESSION — Pi 4 GENET TX bring-up

## ここまでで動いていること (HEAD `e99008d`)
- BCM2711 GENET v5 controller alive (SYS_REV_CTRL = 0x06000000)
- UMAC enabled: UMAC_CMD = 0x00000003 (TX_EN | RX_EN) reads back OK
- BCM54213PE PHY (PHYSID1=0x600D, PHYSID2=0x84A2), link UP
- TDMA register layout 確定 (stride 0x40, ring 16 = 0x4400, top = 0x4440)
- TX descriptor word 0 = 0x003CD07F (length 60 in bits 31:16, SOP|EOP|OW|CRC|QTAG in bits 15:0)
- TDMA_RING_CFG bit 16 enabled, UMAC TX+RX enabled
- shell window + soft keyboard + cursor + 1280×960 virtual desktop も完成

## 動かない: CONS_INDEX が advance しない
症状:
- PROD_INDEX = 1 を書いても CONS_INDEX = 0 のまま
- DMA_STATUS = 0
- TDMA_CTRL を `0x20001` (bit 0 + bit 17) に書くと、HW は `0x21000` (bit 12 + bit 17) を返す
  - bit 0 (我々の DMA_EN) が即クリアされる
  - bit 12 が HW 由来で立っている
- Mac 側 tcpdump で broadcast ARP が一切観測されない

## 次セッションでやること

### 1. Linux source 精読
`drivers/net/ethernet/broadcom/genet/bcmgenet.c` を以下の順で読み:

- `bcmgenet_init_dma()` (DMA bring-up シーケンス)
- `bcmgenet_enable_dma()` (DMA_CTRL の bit 構成と書き方)
- `bcmgenet_init_tx_ring()` (ring register 設定順序)
- `bcmgenet_xmit()` (実際の TX path — descriptor + PROD_INDEX bump)

特に確認したい:
- `DMA_EN` bit 位置 (GENET v5 でも本当に bit 0 か?)
- per-ring data path enable の shift (`DMA_RING_BUF_EN_SHIFT`)
- ring 16 (DESC_INDEX) の特殊扱いの有無
- TDMA_CTRL の `0x21000` の bit 12 が何を意味するか

### 2. 可能性が高い missing piece
- **GENET_INTRL2_0/1 mask register クリア** — IRQ 経路を mask しないと DMA も止まる可能性
- **TBUF_CTRL** programming — TX buffer ring の独立設定
- **TX BURST size** (TDMA_SCB_BURST_SIZE) が 8 で正しいか
- **RGMII RXC/TXC delay** が EXT_RGMII_OOB_CTRL の他 bit で必要
- **memory cache coherency** — MMU off だが Device-nGnRnE 属性で writeはバッファされ得る

### 3. ハードリセット試行
SYS_RBUF/TBUF_FLUSH_CTRL に **1 を書いてから 0 に戻す** (パルスでリセット)
が必要かもしれない。我々は 0 のままにしているだけ。

### 4. 並行調査
- U-Boot for Pi 4 (`drivers/net/bcmgenet.c`) も別実装として参考
- circle/lib/bcmgenet.cpp (rsta2/circle、Pi 4 サポート) も参考
- Raspberry Pi firmware ブートローダのソース (公開分)

## 次セッション目標
1. TX 1 frame 送信成功 (Mac tcpdump で broadcast ARP 確認)
2. NET-D: RX 1 frame 受信
3. NET-E: ARP responder (xinu-raz のコード移植 or 自作)
