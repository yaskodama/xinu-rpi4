// device/genet/genet.c — BCM2711 GENET Ethernet driver, phase NET-A.
//
// Block layout (from Linux bcmgenet.h, GENET v5 used on BCM2711):
//
//   GENET_BASE + 0x0000   SYS block      (revision, port ctrl, RBUF flush)
//   GENET_BASE + 0x0040   GR_BRIDGE      (bridge ctrl)
//   GENET_BASE + 0x0080   EXT            (ext-config / PHY isolation)
//   GENET_BASE + 0x0200   INTRL2_0       (interrupt level-2, set 0)
//   GENET_BASE + 0x0240   INTRL2_1       (interrupt level-2, set 1)
//   GENET_BASE + 0x0300   RBUF           (RX buffer ctrl)
//   GENET_BASE + 0x0600   TBUF           (TX buffer ctrl)
//   GENET_BASE + 0x0800   UMAC           (UniMAC core)
//   GENET_BASE + 0x1000   HFB            (hardware filter)
//   GENET_BASE + 0x2040   TDMA ring 0    (TX DMA descriptors)
//   GENET_BASE + 0x4000   RDMA ring 0    (RX DMA descriptors)
//
// NET-A only reads SYS_REV_CTRL and SYS_PORT_CTRL.  A live BCM2711
// reports SYS_REV_CTRL ≈ 0x06000000 (major.minor.patch encoded).

#include "genet.h"
#include "uart.h"
#include "mbox.h"

#ifdef GENET_BASE

#define GENET_REG(off)         (*(volatile unsigned int *)(GENET_BASE + (off)))
#define SYS_REV_CTRL           0x000
#define SYS_PORT_CTRL          0x004
#define SYS_RBUF_FLUSH_CTRL    0x008
#define SYS_TBUF_FLUSH_CTRL    0x00C

/* UMAC (UniMAC core).  Inside Linux this is GENET_BASE + 0x800
 * with internal register offsets numbered from 0.  Useful subset
 * for this phase: */
#define UMAC_BASE              0x800
#define UMAC_HD_BKP_CTRL       (UMAC_BASE + 0x004)
#define UMAC_CMD               (UMAC_BASE + 0x008)
#define UMAC_MAC0              (UMAC_BASE + 0x00C)   /* MAC[5..2] big-end */
#define UMAC_MAC1              (UMAC_BASE + 0x010)   /* MAC[1..0] high half */
#define UMAC_MAX_FRAME_LEN     (UMAC_BASE + 0x014)
#define UMAC_MIB_CTRL          (UMAC_BASE + 0x580)
#define UMAC_MIB_RESET_RX      (1u << 0)
#define UMAC_MIB_RESET_RUNT    (1u << 1)
#define UMAC_MIB_RESET_TX      (1u << 2)

#define CMD_TX_EN              (1u << 0)
#define CMD_RX_EN              (1u << 1)
#define CMD_LCL_LOOP_EN        (1u << 15)
#define CMD_SW_RESET           (1u << 13)
#define RBUF_CTRL              0x300

/* MDIO command register inside UMAC: UMAC_BASE + 0x614 = 0xE14 */
#define UMAC_MDIO_CMD          (UMAC_BASE + 0x614)

/* TX DMA block layout (GENET v4/v5 — Linux bcmgenet_dma_ring_regs_v4).
 *   GENET_BASE + 0x4000 (TDMA area, 8 KiB)
 *     17 per-ring blocks of 0x40 bytes each (rings 0..15 user, 16 default)
 *     followed by top-level DMA control registers
 *   Default ring 16 base = 0x4000 + 16 * 0x40 = 0x4400
 *   Top-level DMA registers at 0x4000 + 17 * 0x40 = 0x4440
 */
#define GENET_TX_OFF                0x4000
#define DMA_RING_STRIDE             0x40
#define TX_DEFAULT_RING             16
#define TX_RING_BASE                (GENET_TX_OFF + DMA_RING_STRIDE * TX_DEFAULT_RING)
#define TX_DMA_TOP                  (GENET_TX_OFF + DMA_RING_STRIDE * 17)

/* Per-ring TX register offsets — GENET v4/v5 layout from Linux. */
#define TDMA_READ_PTR_LO            0x00
#define TDMA_READ_PTR_HI            0x04
#define TDMA_CONS_INDEX             0x08
#define TDMA_PROD_INDEX             0x0C
#define TDMA_RING_BUF_SIZE          0x10
#define TDMA_START_ADDR_LO          0x14
#define TDMA_START_ADDR_HI          0x18
#define TDMA_END_ADDR_LO            0x1C
#define TDMA_END_ADDR_HI            0x20
#define TDMA_MBUF_DONE_THRESH       0x24
#define TDMA_FLOW_PERIOD            0x28
#define TDMA_WRITE_PTR_LO           0x2C
#define TDMA_WRITE_PTR_HI           0x30

/* Top-level DMA control register offsets (relative to TX_DMA_TOP) */
#define TDMA_RING_CFG               0x00     /* bit per ring = enable */
#define TDMA_CTRL                   0x04     /* bit 0 = TDMA enable */
#define TDMA_STATUS                 0x08
#define TDMA_SCB_BURST_SIZE         0x0C

/* EXT block (offset 0x80) — RGMII out-of-band control. */
#define GENET_EXT_OFF               0x80
#define EXT_RGMII_OOB_CTRL          (GENET_EXT_OFF + 0x0C)
#define EXT_OOB_DISABLE             (1u << 5)
#define EXT_RGMII_MODE_EN           (1u << 6)
#define EXT_ID_MODE_DIS             (1u << 16)
#define EXT_RGMII_LINK              (1u << 4)

/* SYS_PORT_CTRL bits */
#define SYS_PORT_MODE_EXT_GPHY      3        /* external 1G PHY */

/* TX descriptor word 0 layout (BCM2711 GENET):
 *   bits 31:16 — length (bytes)
 *   bits 15:0  — status flags
 * Status flags (in low 16 bits):
 *   bit 15: SOP (start of packet)
 *   bit 14: EOP (end of packet)
 *   bit 13: WRAP
 *   bit 12: TX_OW (descriptor owned by hardware)
 *   bit 6 : TX_APPEND_CRC
 *   bits 5:0 — QTAG
 */
#define DMA_BUFLENGTH_SHIFT         16
#define DMA_SOP                     (1u << 15)
#define DMA_EOP                     (1u << 14)
#define DMA_TX_OWN                  (1u << 12)
#define DMA_TX_APPEND_CRC           (1u << 6)
#define DMA_TX_QTAG_SHIFT           0
#define DMA_TX_DEFAULT_QTAG         0x3F     /* 6 bits */
#define MDIO_START_BUSY        (1u << 29)
#define MDIO_READ_FAIL         (1u << 28)
#define MDIO_RD                (1u << 27)
#define MDIO_WR                (1u << 26)
#define MDIO_PHY_ID_SHIFT      21
#define MDIO_REG_SHIFT         16
#define MDIO_DATA_MASK         0xFFFFu

#define PHY_ID_BCM54213PE      1  /* MDIO address on Pi 4 */

/* IEEE 802.3 MII registers we care about */
#define MII_BMCR               0x00
#define MII_BMSR               0x01
#define MII_PHYSID1            0x02
#define MII_PHYSID2            0x03
#define MII_ANAR               0x04
#define MII_ANLPAR             0x05
#define BMSR_LSTATUS           (1u << 2)
#define BMSR_ANEGCOMPLETE      (1u << 5)

static void puts_hex32(unsigned int v)
{
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int nyb = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[10] = 0;
    uart_puts(buf);
}

unsigned int genet_sys_rev(void)         { return GENET_REG(SYS_REV_CTRL); }
unsigned int genet_sys_port_ctrl(void)   { return GENET_REG(SYS_PORT_CTRL); }

void genet_get_mac(unsigned char mac[6])
{
    /* UMAC stores the MAC in two 32-bit slots:
     *   UMAC_MAC0 = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]
     *   UMAC_MAC1 = (mac[0] <<  8) |  mac[1]
     * — i.e. big-endian byte order with bytes 4..5 in mac0 LSBs.
     * (Linux: bcmgenet_umac_set_hw_addr / get_hw_addr.) */
    unsigned int m0 = GENET_REG(UMAC_MAC0);
    unsigned int m1 = GENET_REG(UMAC_MAC1);
    mac[0] = (unsigned char)((m1 >> 8)  & 0xFF);
    mac[1] = (unsigned char)( m1        & 0xFF);
    mac[2] = (unsigned char)((m0 >> 24) & 0xFF);
    mac[3] = (unsigned char)((m0 >> 16) & 0xFF);
    mac[4] = (unsigned char)((m0 >> 8)  & 0xFF);
    mac[5] = (unsigned char)( m0        & 0xFF);
}

static void genet_set_mac(const unsigned char mac[6])
{
    unsigned int m0 = ((unsigned int)mac[2] << 24)
                    | ((unsigned int)mac[3] << 16)
                    | ((unsigned int)mac[4] <<  8)
                    |  (unsigned int)mac[5];
    unsigned int m1 = ((unsigned int)mac[0] <<  8) | (unsigned int)mac[1];
    GENET_REG(UMAC_MAC0) = m0;
    GENET_REG(UMAC_MAC1) = m1;
}

/* VC mailbox tag 0x00010003 (get-board-mac-address) — firmware
 * reads the OTP and returns the canonical 6-byte MAC.  Reliable
 * even if UMAC_MAC0/1 were cleared by a SW reset. */
static int genet_get_mac_via_mailbox(unsigned char mac[6])
{
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;
    buf[1] = 0;
    buf[2] = 0x00010003u;
    buf[3] = 6;              /* response value buffer size in bytes */
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    if (mbox_call(buf) != 0) return -1;
    /* Response layout: buf[5] = mac[0..3] (little-endian, byte 0 at LSB),
     *                  buf[6] = mac[4..5]. */
    mac[0] = (unsigned char)( buf[5]        & 0xFF);
    mac[1] = (unsigned char)((buf[5] >>  8) & 0xFF);
    mac[2] = (unsigned char)((buf[5] >> 16) & 0xFF);
    mac[3] = (unsigned char)((buf[5] >> 24) & 0xFF);
    mac[4] = (unsigned char)( buf[6]        & 0xFF);
    mac[5] = (unsigned char)((buf[6] >>  8) & 0xFF);
    return 0;
}

static void puts_mac(const unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) {
        unsigned char b = mac[i];
        unsigned char hi = (unsigned char)((b >> 4) & 0xF);
        unsigned char lo = (unsigned char)(b & 0xF);
        uart_putc((char)(hi < 10 ? '0' + hi : 'a' + hi - 10));
        uart_putc((char)(lo < 10 ? '0' + lo : 'a' + lo - 10));
        if (i < 5) uart_putc(':');
    }
}

static inline void udelay_busy(unsigned long us)
{
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000000UL) * us;
    unsigned long start, now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
    do {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < target);
}

static int mdio_read(unsigned phy_id, unsigned reg)
{
    /* Issue: PHY_ID + REG, then assert START_BUSY.  HW clears it
     * when the access completes (~70 µs at the MDIO clock).  Bound
     * the wait via CNTPCT_EL0 — never spin on bare MMIO polls,
     * because if MDIO clocks aren't running the bit never clears
     * and the kernel hangs. */
    unsigned int cmd = ((phy_id & 0x1F) << MDIO_PHY_ID_SHIFT)
                     | ((reg    & 0x1F) << MDIO_REG_SHIFT)
                     |  MDIO_RD;
    GENET_REG(UMAC_MDIO_CMD) = cmd;
    unsigned int reg2 = GENET_REG(UMAC_MDIO_CMD);
    reg2 |= MDIO_START_BUSY;
    GENET_REG(UMAC_MDIO_CMD) = reg2;

    /* Timeout: 50 ms in CNTPCT ticks. */
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000UL) * 50UL;
    unsigned long start, now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));

    while (1) {
        unsigned int v = GENET_REG(UMAC_MDIO_CMD);
        if (!(v & MDIO_START_BUSY)) {
            if (v & MDIO_READ_FAIL) return -1;
            return (int)(v & MDIO_DATA_MASK);
        }
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
        if (now - start >= target) return -2;   /* timeout */
    }
}

/* forward decl */
static void genet_send_one_arp(const unsigned char src_mac[6]);

static void umac_soft_reset(void)
{
    /* Mirrors Linux bcmgenet_umac_reset() but skips the RBUF_CTRL
     * preamble (it was hanging us last build).  The LCL_LOOP_EN bit
     * keeps the rxclk stable during the SW reset window — without
     * it the UMAC can leave reset in a state where subsequent
     * register writes (e.g. UMAC_MAC0) stall the bus. */
    GENET_REG(UMAC_CMD) = 0;
    GENET_REG(UMAC_CMD) = CMD_SW_RESET | CMD_LCL_LOOP_EN;
    for (volatile int i = 0; i < 1000; i++) { }
    GENET_REG(UMAC_CMD) = 0;
}

void genet_init(void)
{
    uart_puts("genet: BCM2711 GENET base = ");
    puts_hex32((unsigned int)GENET_BASE);
    uart_puts("\n");

    unsigned int rev   = GENET_REG(SYS_REV_CTRL);
    unsigned int pctl  = GENET_REG(SYS_PORT_CTRL);
    unsigned int rflsh = GENET_REG(SYS_RBUF_FLUSH_CTRL);
    unsigned int tflsh = GENET_REG(SYS_TBUF_FLUSH_CTRL);

    uart_puts("  SYS_REV_CTRL        = "); puts_hex32(rev);   uart_puts("\n");
    uart_puts("  SYS_PORT_CTRL       = "); puts_hex32(pctl);  uart_puts("\n");
    uart_puts("  SYS_RBUF_FLUSH_CTRL = "); puts_hex32(rflsh); uart_puts("\n");
    uart_puts("  SYS_TBUF_FLUSH_CTRL = "); puts_hex32(tflsh); uart_puts("\n");

    if (rev == 0 || rev == 0xFFFFFFFFu) {
        uart_puts("genet: controller MMIO not responding\n");
        return;
    }
    /* GENET v5 on BCM2711: top byte of SYS_REV_CTRL is the GENET major. */
    unsigned int major = (rev >> 24) & 0xFFu;
    uart_puts("genet: GENET major rev = ");
    puts_hex32(major);
    uart_puts("  (expected 0x06 on Pi 4)\n");

    /* NET-B — read MAC *before* the SW reset (firmware may have
     * preloaded UMAC_MAC0/1), then reset UMAC, then write the MAC
     * back so RX filtering and DA insertion work. */
    unsigned char mac_pre[6];
    genet_get_mac(mac_pre);
    uart_puts("genet: MAC pre-reset = ");
    puts_mac(mac_pre);
    uart_puts("\n");

    /* MAC fallback chain:
     *   1. firmware-loaded UMAC_MAC0/1 (when firmware filled them)
     *   2. otherwise local-admin synthetic 02:00:00:00:00:01
     *
     * Note: VC mailbox 0x00010003 (get-board-mac-address) used to
     * be tried here but calling it then immediately writing UMAC
     * registers triggered a bus hang on Pi 4 — possibly some bus
     * fence state the mailbox leaves behind.  Skipping it lets the
     * UMAC reset proceed cleanly. */
    int is_zero = 1;
    for (int i = 0; i < 6; i++) if (mac_pre[i]) { is_zero = 0; break; }
    unsigned char mac[6];
    if (!is_zero) {
        for (int i = 0; i < 6; i++) mac[i] = mac_pre[i];
    } else {
        uart_puts("genet: pre-reset MAC is all-zero; using local-admin 02:00:00:00:00:01\n");
        mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x00;
        mac[3] = 0x00; mac[4] = 0x00; mac[5] = 0x01;
    }

    /* SKIP umac_soft_reset and genet_set_mac on Pi 4.
     *
     * Empirically: with Pi 4 firmware's GENET state, writing
     * UMAC_MAC0 stalls the AXI bus indefinitely.  Firmware has
     * already configured UMAC to a usable state, so we don't
     * actually need to reset it here.  When we eventually need
     * the source MAC we'll put it in the Ethernet header
     * directly rather than relying on UMAC's DA insertion. */
    uart_puts("genet: skipping UMAC reset + MAC write (firmware-initialised)\n");
    (void)mac;

    /* NET-C1 — read BCM54213PE PHY via MDIO so we know the link
     * is up before we bother with TDMA ring init.  PHY ID is 1
     * on Pi 4 (only one PHY on the bus).
     *
     * Note: any single mdio_read() that times out returns -2; we
     * print that as -0x2 so the failure mode is visible rather
     * than freezing the kernel. */
    uart_puts("genet: probing PHY 1 ...\n");
    int phyid1 = mdio_read(PHY_ID_BCM54213PE, MII_PHYSID1);
    uart_puts("genet/phy: PHYSID1 = "); puts_hex32((unsigned)phyid1); uart_puts("\n");
    int phyid2 = mdio_read(PHY_ID_BCM54213PE, MII_PHYSID2);
    uart_puts("genet/phy: PHYSID2 = "); puts_hex32((unsigned)phyid2); uart_puts("\n");
    int bmcr   = mdio_read(PHY_ID_BCM54213PE, MII_BMCR);
    uart_puts("genet/phy: BMCR    = "); puts_hex32((unsigned)bmcr);   uart_puts("\n");
    int bmsr   = mdio_read(PHY_ID_BCM54213PE, MII_BMSR);
    uart_puts("genet/phy: BMSR    = "); puts_hex32((unsigned)bmsr);   uart_puts("\n");
    /* BCM54213PE OUI 0x001818 should appear in phyid1/phyid2:
     * PHYSID1 = 0x600D, PHYSID2 = 0x84A2 (BCM54213PE). */
    if (bmsr >= 0) {
        uart_puts("genet/phy: link  = ");
        uart_puts((bmsr & BMSR_LSTATUS) ? "UP\n" : "DOWN\n");
        uart_puts("genet/phy: aneg  = ");
        uart_puts((bmsr & BMSR_ANEGCOMPLETE) ? "complete\n" : "in progress\n");
    }

    /* BMSR.Link Status is *latched low* — the first read clears the
     * latched bit and the second shows the actual current state.
     * Poll a couple of times so we don't miss a link that comes
     * up a fraction of a second after the kernel banner. */
    int link_up = 0;
    for (int t = 0; t < 3; t++) {
        udelay_busy(200000UL);   /* 200 ms */
        int bmsr2 = mdio_read(PHY_ID_BCM54213PE, MII_BMSR);
        uart_puts("genet/phy: BMSR (poll ");
        uart_putc((char)('1' + t));
        uart_puts(") = ");
        puts_hex32((unsigned)bmsr2);
        if (bmsr2 >= 0 && (bmsr2 & BMSR_LSTATUS)) {
            uart_puts("  link=UP\n");
            link_up = 1;
            break;
        }
        uart_puts("  link=DOWN\n");
    }
    (void)link_up;

    if (!link_up) {
        uart_puts("genet: link still DOWN — skipping TX setup\n");
        return;
    }

    /* ====================================================== *
     * NET-C3 — send one broadcast ARP frame via TDMA ring 16 *
     * ====================================================== */
    genet_send_one_arp(mac);
}

/* TX descriptor (3 words = 12 bytes) lives in main memory and is
 * shared with the GENET DMA engine.  Compile-time-initialised so
 * we don't go through any runtime memset path that might depend
 * on libc.  Same for tx_buf — pre-built ARP request frame, src
 * MAC patched in place at call time. */
static volatile unsigned int __attribute__((aligned(64))) tx_desc[3] = { 0, 0, 0 };

static unsigned char __attribute__((aligned(64))) tx_buf[64] = {
    /* Ethernet header */
    0xff,0xff,0xff,0xff,0xff,0xff,                  /* dst broadcast */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* src (patched) */
    0x08,0x06,                                       /* EtherType ARP */
    /* ARP payload */
    0x00,0x01,                                       /* HTYPE Ethernet */
    0x08,0x00,                                       /* PTYPE IPv4 */
    0x06,                                            /* HLEN */
    0x04,                                            /* PLEN */
    0x00,0x01,                                       /* OPER request */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* SHA (patched) */
    0x00,0x00,0x00,0x00,                             /* SPA 0.0.0.0 */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* THA */
    192,168,1,1,                                     /* TPA */
    /* zero pad to 60 bytes */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static void genet_send_one_arp(const unsigned char src_mac[6])
{
    uart_puts("genet/tx: NET-C3 sending one broadcast ARP via ring 16\n");

    /* Step 1 — skip src-MAC patch (tx_buf is hanging on writes).
     * Use the all-zero src MAC baked in at compile time for now. */
    (void)src_mac;
    uart_puts("genet/tx: skipped src MAC patch (using compile-time zeros)\n");
    unsigned int len = 60;
    uart_puts("genet/tx: frame length = ");
    puts_hex32(len);
    uart_puts("\n");

    /* Step 2 — populate TX descriptor.  Word 0 carries length in
     * the low 16 bits and SOP+EOP+QTAG+CRC in the upper bits. */
    unsigned long buf_pa = (unsigned long)tx_buf;
    tx_desc[0] = ((unsigned int)len << DMA_BUFLENGTH_SHIFT)
               | DMA_SOP | DMA_EOP | DMA_TX_OWN | DMA_TX_APPEND_CRC
               | (DMA_TX_DEFAULT_QTAG << DMA_TX_QTAG_SHIFT);
    tx_desc[1] = (unsigned int)(buf_pa & 0xFFFFFFFFu);
    tx_desc[2] = (unsigned int)((buf_pa >> 32) & 0xFFFFFFFFu);
    __asm__ volatile ("dsb sy" ::: "memory");

    uart_puts("genet/tx: desc[0] = "); puts_hex32(tx_desc[0]); uart_puts("\n");
    uart_puts("genet/tx: desc[1] = "); puts_hex32(tx_desc[1]); uart_puts("\n");
    uart_puts("genet/tx: desc[2] = "); puts_hex32(tx_desc[2]); uart_puts("\n");

    /* Step 3 — configure TDMA ring 16.
     *
     * IMPORTANT: START_ADDR / END_ADDR / WRITE_PTR / READ_PTR are
     * in 32-bit *word* units (Linux bcmgenet does `addr / 4`).
     * Using byte addresses leaves the DMA engine confused and
     * CONS_INDEX never advances. */
    unsigned long desc_pa = (unsigned long)tx_desc;
    unsigned long start_w = desc_pa / 4UL;
    unsigned long end_w   = (desc_pa + 3UL * 4UL) / 4UL - 1UL;  /* 1 desc = 3 words */

    GENET_REG(TX_RING_BASE + TDMA_RING_BUF_SIZE)     = (1u << 16) | 2048u;
    GENET_REG(TX_RING_BASE + TDMA_START_ADDR_LO)     = (unsigned int)(start_w & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_START_ADDR_HI)     = (unsigned int)((start_w >> 32) & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_END_ADDR_LO)       = (unsigned int)(end_w & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_END_ADDR_HI)       = (unsigned int)((end_w >> 32) & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_MBUF_DONE_THRESH)  = 1;
    GENET_REG(TX_RING_BASE + TDMA_FLOW_PERIOD)       = 0;
    GENET_REG(TX_RING_BASE + TDMA_WRITE_PTR_LO)      = (unsigned int)(start_w & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_WRITE_PTR_HI)      = (unsigned int)((start_w >> 32) & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_READ_PTR_LO)       = (unsigned int)(start_w & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_READ_PTR_HI)       = (unsigned int)((start_w >> 32) & 0xFFFFFFFFu);
    GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX)        = 0;
    GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX)        = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: TDMA ring 16 configured (start_w=");
    puts_hex32((unsigned)start_w);
    uart_puts(", end_w=");
    puts_hex32((unsigned)end_w);
    uart_puts(")\n");

    /* Step 4-pre: prerequisites that Linux bcmgenet does before
     * enabling DMA / UMAC.  Without these the UMAC_CMD write
     * silently goes to /dev/null on Pi 4 (firmware leaves GENET
     * in flush state). */

    uart_puts("genet/tx: SYS_PORT_CTRL  = ");
    puts_hex32(GENET_REG(SYS_PORT_CTRL));
    uart_puts("\n");
    GENET_REG(SYS_PORT_CTRL) = SYS_PORT_MODE_EXT_GPHY;

    /* Clear RBUF/TBUF flush so the UMAC sees a stable state. */
    GENET_REG(SYS_RBUF_FLUSH_CTRL) = 0;
    GENET_REG(SYS_TBUF_FLUSH_CTRL) = 0;

    /* EXT block — enable RGMII mode, deassert OOB disable, set
     * ID mode disable (RX internal delay handled by PHY). */
    unsigned int ext = GENET_REG(EXT_RGMII_OOB_CTRL);
    uart_puts("genet/tx: EXT_RGMII_OOB before = "); puts_hex32(ext); uart_puts("\n");
    ext |= EXT_RGMII_MODE_EN | EXT_ID_MODE_DIS;
    ext &= ~EXT_OOB_DISABLE;
    GENET_REG(EXT_RGMII_OOB_CTRL) = ext;
    uart_puts("genet/tx: EXT_RGMII_OOB after  = ");
    puts_hex32(GENET_REG(EXT_RGMII_OOB_CTRL));
    uart_puts("\n");

    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 4a — DMA burst size.  Linux writes 8 (= 4 cache lines)
     * here; without this the DMA engine sometimes won't even
     * fetch descriptors. */
    GENET_REG(TX_DMA_TOP + TDMA_SCB_BURST_SIZE) = 8u;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 4b — enable ring 16 in DMA_RING_CFG, then enable TDMA.
     *
     * DMA_CTRL has TWO classes of enable bits, not just bit 0:
     *   bit 0     = global DMA enable
     *   bits 1..17 = per-ring data-path enable (ring N at bit N+1)
     * Linux's bcmgenet_enable_dma() sets both.  Without the per-
     * ring bit the data path stays gated even with DMA_RING_CFG. */
    GENET_REG(TX_DMA_TOP + TDMA_RING_CFG) = (1u << TX_DEFAULT_RING);
    unsigned int dma_ctrl = 1u | (1u << (TX_DEFAULT_RING + 1));   /* global + ring16 = 0x20001 */
    GENET_REG(TX_DMA_TOP + TDMA_CTRL) = dma_ctrl;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: TDMA_RING_CFG = ");
    puts_hex32(GENET_REG(TX_DMA_TOP + TDMA_RING_CFG));
    uart_puts("\n");
    uart_puts("genet/tx: TDMA_CTRL     = ");
    puts_hex32(GENET_REG(TX_DMA_TOP + TDMA_CTRL));
    uart_puts("\n");

    /* Step 5 — enable UMAC TX + RX path.  Direct overwrite (not
     * RMW) so any leftover SW_RESET / LCL_LOOP bits get cleared.
     * Print before+after so we can see whether the write stuck. */
    unsigned int cmd_pre = GENET_REG(UMAC_CMD);
    uart_puts("genet/tx: UMAC_CMD before = "); puts_hex32(cmd_pre); uart_puts("\n");
    GENET_REG(UMAC_CMD) = CMD_TX_EN | CMD_RX_EN;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: UMAC_CMD after  = ");
    puts_hex32(GENET_REG(UMAC_CMD));
    uart_puts("\n");

    /* Step 6 — bump PROD_INDEX to hand the descriptor to the
     * DMA engine.  HW will fetch the descriptor, DMA the buffer,
     * and emit the frame on the PHY.  CONS_INDEX will advance to
     * 1 when transmission completes. */
    GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX) = 1;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: PROD_INDEX=1, waiting for CONS_INDEX...\n");

    /* Step 7 — poll for completion with 200 ms timeout. */
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000UL) * 200UL;
    unsigned long start, now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
    unsigned int cons = 0;
    while (1) {
        cons = GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX);
        if (cons >= 1) break;
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
        if (now - start >= target) break;
    }
    /* Re-print descriptor here so the value survives even after
     * 18 lines of shell-window scroll. */
    uart_puts("genet/tx: desc[0] re = "); puts_hex32(tx_desc[0]); uart_puts("\n");
    uart_puts("genet/tx: CONS_INDEX = "); puts_hex32(cons);
    uart_puts((cons >= 1) ? "  (sent OK)\n" : "  (TX timeout)\n");
    /* compact diag */
    if (cons < 1) {
        uart_puts("genet/tx: DMA_S/CTRL = ");
        puts_hex32(GENET_REG(TX_DMA_TOP + TDMA_STATUS));
        uart_puts(" / ");
        puts_hex32(GENET_REG(TX_DMA_TOP + TDMA_CTRL));
        uart_puts("\n");
    }
}

#endif /* GENET_BASE */
