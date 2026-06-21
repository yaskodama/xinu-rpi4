// device/usb/xhci/xhci.c — VL805 xHCI driver, phase XHCI-A (probe).
//
// Today this file:
//   1. Tries VC mailbox tag 0x00030058 (notify-xhci-reset).  On Pi 4
//      firmware this re-runs the VL805 bring-up sequence — Linux's
//      xhci-pci.c does the same thing when it sees vendor 0x1106
//      device 0x3483.
//   2. Probes the BCM2711 PCIe-1 controller MMIO at several offsets
//      (revision, status, link cap, root-port vendor) so we can see
//      from the shell-window log which registers respond and which
//      don't.
//
// All output goes through uart_puts() so it shows up both on the
// (absent) UART cable and in the on-screen shell window.

#include "xhci.h"
#include "uart.h"
#include "mbox.h"

#ifdef PCIE_BASE

#define PCIE_REG(off)         (*(volatile unsigned int *)(PCIE_BASE + (off)))
#define PCIE_RC_CFG_VENDOR    0x0000  /* root-complex CFG: PCI vendor/device */
#define PCIE_RC_CFG_CLASS     0x0008
#define PCIE_MISC_CTRL        0x4008
#define PCIE_MISC_REVISION    0x406C
#define PCIE_MISC_STATUS      0x4068
#define PCIE_MISC_HARD_DBG    0x4204
#define PCIE_RC_CFG_LINK_CAP  0x04DC

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

static int xhci_notify_reset(void)
{
    /* Property tag 0x00030058 (notify-xhci-reset): make the VPU (re)load the
     * VL805 firmware after a PCI reset.  Per Linux drivers/reset/reset-
     * raspberrypi.c the dev address is encoded PCI_BUS<<20|SLOT<<15|FUNC<<12;
     * VL805 is hardwired at bus1/dev0/fn0 => 0x100000.  (devid=0 is a no-op;
     * an earlier 0x100000 "hang" was with the link not yet trained — Linux
     * uses exactly this value once the RC link is up.) */
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;
    buf[1] = 0;
    buf[2] = 0x00030058U;        /* notify-xhci-reset */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0x00100000U;        /* devid: bus1<<20|slot0<<15|func0<<12 = VL805 */
    buf[6] = 0;
    buf[7] = 0;
    return mbox_call(buf);
}

unsigned int xhci_pcie_revision(void)
{
    return PCIE_REG(PCIE_MISC_REVISION);
}

static void dump_pcie_probe(const char *label)
{
    uart_puts("xhci: --- "); uart_puts(label); uart_puts(" ---\n");
    uart_puts("  vendor[0x000] = ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_VENDOR));   uart_puts("\n");
    uart_puts("  class [0x008] = ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_CLASS));    uart_puts("\n");
    uart_puts("  ctrl  [0x4008]= ");  puts_hex32(PCIE_REG(PCIE_MISC_CTRL));       uart_puts("\n");
    uart_puts("  stat  [0x4068]= ");  puts_hex32(PCIE_REG(PCIE_MISC_STATUS));     uart_puts("\n");
    uart_puts("  rev   [0x406C]= ");  puts_hex32(PCIE_REG(PCIE_MISC_REVISION));   uart_puts("\n");
    uart_puts("  dbg   [0x4204]= ");  puts_hex32(PCIE_REG(PCIE_MISC_HARD_DBG));   uart_puts("\n");
    uart_puts("  lcap  [0x04DC]= ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_LINK_CAP)); uart_puts("\n");
}

void xhci_init(void)
{
    uart_puts("xhci: BCM2711 PCIe-1 base = ");
    puts_hex32((unsigned int)PCIE_BASE);
    uart_puts("\n");

    dump_pcie_probe("pre-mailbox");

    int rc = xhci_notify_reset();
    uart_puts("xhci: notify-xhci-reset rc = ");
    puts_hex32((unsigned int)rc);
    uart_puts("\n");

    dump_pcie_probe("post-mailbox");
}

/* On-demand /xhci-reset HTTP route: do the VC mailbox call separately so we
 * can see whether it returns or hangs (the boot-time variant wedged the box). */
int xhci_notify_reset_call(void) { return xhci_notify_reset(); }

/* Firmware-proxied peripheral register read/write via VC mailbox.  Safer than
 * direct MMIO when the controller may be clock-gated (firmware handles the bus
 * access; we can't hang the AXI from our side).  Tags:
 *   GET_PERIPH_REG = 0x00030045   in: addr; out: value
 *   SET_PERIPH_REG = 0x00038045   in: addr + value
 * Found in raspberrypi-firmware.h.  Returns -1 on mailbox failure, 0 on
 * success; value written into *out for GET. */
int xhci_periph_read(unsigned int addr, unsigned int *out, unsigned int *resp)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[12];
    buf[0] = 48;            /* total size (12*4 = 48 bytes) */
    buf[1] = 0;             /* request code */
    buf[2] = 0x00030045U;   /* GET_PERIPH_REG */
    buf[3] = 8;             /* value buf size: addr (in) + value (out) */
    buf[4] = 0;             /* req/resp */
    buf[5] = addr;          /* IN: peripheral register address */
    buf[6] = 0;             /* OUT: value */
    buf[7] = 0;             /* end tag */
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (mbox_call(buf) != 0) return -1;
    *out = buf[6];
    if (resp) *resp = buf[4];   /* 0x80000000+len = handled, 0 = ignored */
    return 0;
}

/* Sanity-check: GET_FIRMWARE_REVISION — known-good tag. */
int xhci_firmware_revision(unsigned int *out, unsigned int *resp)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;            /* 8 u32s = 32 bytes */
    buf[1] = 0;
    buf[2] = 0x00000001U;   /* GET_FIRMWARE_REVISION */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;             /* end tag */
    buf[7] = 0;
    if (mbox_call(buf) != 0) return -1;
    *out = buf[5];
    if (resp) *resp = buf[4];
    return 0;
}

/* Direct CPRMAN MMIO read/write.  CPRMAN is always-on (it controls all the
 * other clocks), so reads/writes don't hang the bus the way PCIe MMIO does.
 * Used to enable the PCIe clock by replaying the exact sequence extracted
 * from start4.elf disasm at vaddr 0xed4995e:
 *   mov r1, 0x5a000036  ; password | SRC=6, ENAB=1, KILL=1
 *   mov r0, 0x7e101000  ; CPRMAN base (VC view; ARM 0xFE101000)
 *   st  r1, (r0+0x128)  ; CTL register
 *   mov r3, 0x5a000000  ; password only
 *   st  r3, (r0+0x12C)  ; DIV register
 * Note: 0x128 is NOT in Linux clk-bcm2835.c (standard CPRMAN map ends earlier
 * for the relevant block), so it's a BCM2711-only addition — strong PCIe
 * candidate.  If wrong, we kill some other clock and the box locks up. */
#define CPRMAN_BASE       0xFE101000UL
#define CPRMAN_PASSWORD   0x5A000000U
#define CM_CTL_ENAB       (1U << 4)
#define CM_CTL_KILL       (1U << 5)
#define CM_CTL_SRC_MASK   0xfU

/* Write 0x5A001000 to CPRMAN+0x1B0 — extracted from start4.elf disasm at
 * vaddr 0xed497e8.  Firmware does this iff config flag "reduced_axi" is set.
 * Bit 12 with password — non-standard format, possibly clock-divider or PCIe
 * AXI gating.  Bare-metal kernels never hit this path; we replay directly. */
int xhci_cprman_pcie_axi_enable(void)
{
    extern void delay_ms(unsigned int ms);
    volatile unsigned int *reg = (volatile unsigned int *)(CPRMAN_BASE + 0x1B0);
    unsigned int before = *reg;
    *reg = CPRMAN_PASSWORD | 0x1000;
    __asm__ volatile ("dsb sy" ::: "memory");
    delay_ms(1);
    unsigned int after = *reg;
    /* Pack before/after into the return value: high 16 = before lo, low 16 = after lo. */
    return (int)((before & 0xFFFF) << 16 | (after & 0xFFFF));
}

/* Direct CPRMAN MMIO read.  CPRMAN is always-on so this won't hang the bus.
 * Returns the raw 32-bit register value at CPRMAN_BASE + offset. */
unsigned int xhci_cprman_read(unsigned int offset)
{
    volatile unsigned int *p = (volatile unsigned int *)(CPRMAN_BASE + offset);
    unsigned int v = *p;
    __asm__ volatile ("dsb sy" ::: "memory");
    return v;
}

/* Configurable CPRMAN clock enable — `src` chooses clock source (1=OSC,
 * 6=PLLD_PER per BCM2835 conventions).  Returns post-write CTL value. */
int xhci_cprman_enable_pcie_src(unsigned int src)
{
    extern void delay_ms(unsigned int ms);
    volatile unsigned int *cm_ctl = (volatile unsigned int *)(CPRMAN_BASE + 0x128);
    volatile unsigned int *cm_div = (volatile unsigned int *)(CPRMAN_BASE + 0x12C);
    src &= CM_CTL_SRC_MASK;
    /* Step 1: stop the clock (KILL=1) before configuring source / divider */
    *cm_ctl = CPRMAN_PASSWORD | CM_CTL_KILL | src;
    __asm__ volatile ("dsb sy" ::: "memory");
    /* Step 2: set divider to 1 (DIV.INT=1, DIV.FRAC=0) */
    *cm_div = CPRMAN_PASSWORD | (1U << 12);
    __asm__ volatile ("dsb sy" ::: "memory");
    /* Step 3: enable (KILL=0, ENAB=1, SRC=src) */
    *cm_ctl = CPRMAN_PASSWORD | CM_CTL_ENAB | src;
    __asm__ volatile ("dsb sy" ::: "memory");
    delay_ms(1);
    return (int)(*cm_ctl & ~CPRMAN_PASSWORD);
}

int xhci_cprman_enable_pcie(void)
{
    return xhci_cprman_enable_pcie_src(6);   /* original: SRC=6 (matches firmware) */
}

/* Replay the FULL firmware PCIe bring-up sequence extracted from start4.elf
 * disassembly at vaddr 0xed4995e onwards.  Multiple peripheral blocks are
 * involved; the simple "write CPRMAN+0x128" pattern only configures the
 * clock register but never makes it BUSY=1 (= clock actually running).
 *
 * Firmware sequence (from disasm):
 *   1. *(0xFEE01000) |= 0x1               — likely "PCIe power domain on"
 *   2. CPRMAN+0x128 = 0x5A000036          — CTL: KILL=1, ENAB=1, SRC=6
 *   3. CPRMAN+0x12C = 0x5A000000          — DIV=0
 *   4. CPRMAN+0x1390 |= 0x5A200001        — bit 21 + bit 0 (power domain ena)
 *   5. *(0xFEC11010) &= ~1                — gating-related (unknown block)
 *   6. *(0xFEC11014) &= ~1                — gating-related
 *   7. CPRMAN+0x108 = 0x5A0003C0          — PLLD CTL
 *   8. CPRMAN+0x20  = 0x5A000040          — PLLD DSI0HSCK
 *   9. CPRMAN+0x34  = 0x5A000000          — PLLD per
 *  10. CPRMAN+0x30  = 0x5A000040          — PLLD core
 *  11. CPRMAN+0x30  = 0x5A000011          — re-write with OSC source enabled
 *
 * Returns a status code packing the final CPRMAN+0x128 value so we can see
 * if BUSY (bit 7) finally gets set. */
int xhci_pcie_clk_full_sequence(void)
{
    extern void delay_ms(unsigned int ms);
    volatile unsigned int *unk_ee = (volatile unsigned int *)0xFEE01000UL;
    volatile unsigned int *unk_ec_10 = (volatile unsigned int *)0xFEC11010UL;
    volatile unsigned int *unk_ec_14 = (volatile unsigned int *)0xFEC11014UL;
    volatile unsigned int *cm = (volatile unsigned int *)CPRMAN_BASE;

    /* Step 1: power domain enable at 0xFEE01000 bit 0 — likely the "ungate
     * PCIe / GENET / PHY power" register.  Read-modify-write to preserve
     * any other state.  CAUTION: if this block is gated too, the read hangs. */
    unsigned int v = *unk_ee;
    *unk_ee = v | 0x1;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 2-3: CPRMAN PCIe CTL + DIV (as before). */
    cm[0x128 / 4] = CPRMAN_PASSWORD | 0x36;          /* CTL: KILL+ENAB+SRC=6 */
    __asm__ volatile ("dsb sy" ::: "memory");
    cm[0x12C / 4] = CPRMAN_PASSWORD;                 /* DIV = 0 */
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 4: CPRMAN+0x1390 |= 0x5A200001 (preserve password, set bits) */
    unsigned int x = cm[0x1390 / 4];
    cm[0x1390 / 4] = (x & 0xFFFFFF) | 0x5A200001U;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 5-6: 0xFEC11010/14 bit-0 clear.  Likely PHY gating disable. */
    unsigned int e10 = *unk_ec_10;
    *unk_ec_10 = e10 & ~1U;
    unsigned int e14 = *unk_ec_14;
    *unk_ec_14 = e14 & ~1U;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Step 7-10: PLLD-related writes.  These are at known CPRMAN offsets
     * (PLLD_CTRL=0x10C in Linux but here 0x108 — VC4 disasm offset includes
     * the entry header maybe). */
    cm[0x108 / 4] = CPRMAN_PASSWORD | 0x3C0;
    __asm__ volatile ("dsb sy" ::: "memory");
    cm[0x20 / 4]  = CPRMAN_PASSWORD | 0x40;
    __asm__ volatile ("dsb sy" ::: "memory");
    cm[0x34 / 4]  = CPRMAN_PASSWORD;
    cm[0x30 / 4]  = CPRMAN_PASSWORD | 0x40;
    cm[0x30 / 4]  = CPRMAN_PASSWORD | 0x11;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Let things settle then read back the PCIe CTL — BUSY bit 7 should
     * be set if clock is actually running. */
    delay_ms(5);
    return (int)(cm[0x128 / 4] & ~CPRMAN_PASSWORD);
}

int xhci_periph_write(unsigned int addr, unsigned int val)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[12];
    buf[0] = 48;
    buf[1] = 0;
    buf[2] = 0x00038045U;   /* SET_PERIPH_REG */
    buf[3] = 8;             /* addr + value */
    buf[4] = 0;
    buf[5] = addr;
    buf[6] = val;
    buf[7] = 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    return mbox_call(buf);
}

/* Bare PCIe-controller register dump into a text buffer; called by the /pcie
 * HTTP route.  Reads that fault are caught by the sync-exception handler
 * (recover_spin) so the box stays alive across iterations. */
static int s_put(char *b, int p, int max, const char *s)
{
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
static int s_puthex32(char *b, int p, int max, unsigned int v)
{
    if (p < max - 1) b[p++] = '0';
    if (p < max - 1) b[p++] = 'x';
    for (int i = 7; i >= 0 && p < max - 1; i--) {
        unsigned int n = (v >> (i * 4)) & 0xF;
        b[p++] = (char)(n < 10 ? '0' + n : 'a' + (n - 10));
    }
    return p;
}
/* Fault-resilient MMIO read/write (system/exception.c).  Returns 0 / -1.
 * safe_mmio cannot catch *hangs* on the AXI bus (no slave response), only
 * synchronous exceptions; but Linux's pcie-brcmstb.c doesn't enable any clock
 * for BCM2711 PCIe (the firmware does at boot), so reads to PCIE_BASE should
 * RESPOND with sane data — at worst the controller is in reset returning 0s. */
extern int safe_mmio_read32(unsigned long addr, unsigned int *out);
extern int safe_mmio_write32(unsigned long addr, unsigned int val);
extern void delay_ms(unsigned int ms);

/* ---- BCM2711 PCIe RC bring-up (from Linux pcie-brcmstb.c brcm_pcie_setup,
 * BCM2711 cfg uses brcm_pcie_perst_set_generic / brcm_pcie_bridge_sw_init_set_generic
 * and pcie_offsets[] = { RGR1=0x9210, EXT_CFG_INDEX=0x9000, EXT_CFG_DATA=0x9004,
 * PCIE_HARD_DEBUG=0x4204, INTR2_CPU=0x4300 }). */
#define PCIE_RGR1_SW_INIT_1              0x9210
#define PCIE_MISC_MISC_CTRL_OFF          0x4008    /* avoid clash w/ existing PCIE_MISC_CTRL */
#define PCIE_MISC_PCIE_CTRL_OFF          0x4064
#define PCIE_MISC_PCIE_STATUS_OFF        0x4068
#define PCIE_MISC_HARD_DEBUG_OFF         0x4204
#define PCIE_EXT_CFG_INDEX               0x9000
#define PCIE_EXT_CFG_DATA                0x8000    /* BCM2711: 0x8000 (was wrongly 0x9004) */

/* Additional brcmstb RC offsets needed for a *usable* link (inbound/outbound
 * windows, interrupt mask, gen, endian) — see Circle bcmpciehostbridge.cpp /
 * rp1pcie.c.  Without these the link can train but MMIO/DMA hang. */
#define PCIE_MISC_CPU_2_PCIE_WIN0_LO     0x400c
#define PCIE_MISC_CPU_2_PCIE_WIN0_HI     0x4010
#define PCIE_MISC_RC_BAR1_LO             0x402c
#define PCIE_MISC_RC_BAR2_LO             0x4034
#define PCIE_MISC_RC_BAR2_HI             0x4038
#define PCIE_MISC_RC_BAR3_LO             0x403c
#define PCIE_MISC_CPU_2_PCIE_WIN0_BLIM   0x4070
#define PCIE_MISC_CPU_2_PCIE_WIN0_BHI    0x4080
#define PCIE_MISC_CPU_2_PCIE_WIN0_LHI    0x4084
#define PCIE_INTR2_CPU_BASE              0x4300
#define PCIE_RC_CFG_VENDOR_SPEC_REG1     0x0188
#define PCIE_CAP_REGS                    0x00ac
#define PCI_EXP_LNKCAP                   0x0c
#define PCI_EXP_LNKCTL2                  0x30

/* CPU view of the PCIe outbound window (CPU 0x6_00000000 -> PCIe 0xC0000000,
 * where VL805's xHCI BAR0 is mapped).  This is the xHCI MMIO base. */
#define VL805_XHCI_MMIO                  0x600000000UL

#define RGR1_SW_INIT_1_INIT_GENERIC_MASK 0x2      /* bit 1: bridge reset */
#define RGR1_SW_INIT_1_PERST_MASK        0x1      /* bit 0: PERST# (generic) */
#define HARD_PCIE_HARD_DEBUG_SERDES_IDDQ 0x08000000  /* bit 27 */
#define HARD_PCIE_HARD_DEBUG_CLKREQ_EN   0x00000002  /* bit 1  */
#define MISC_CTRL_SCB_ACCESS_EN          0x00001000
#define MISC_CTRL_CFG_READ_UR_MODE       0x00002000
#define MISC_CTRL_MAX_BURST_SIZE_MASK    0x00300000
#define MISC_CTRL_SCB0_SIZE_MASK         0xf8000000
#define PCIE_STATUS_DL_ACTIVE            0x20     /* bit 5: data-link layer up */
#define PCIE_STATUS_PHYLINKUP            0x10     /* bit 4: physical link up   */

static int pcie_bridge_sw_init_set(unsigned int val)
{
    unsigned int tmp;
    if (safe_mmio_read32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, &tmp) < 0) return -1;
    tmp = (tmp & ~RGR1_SW_INIT_1_INIT_GENERIC_MASK)
        | ((val << 1) & RGR1_SW_INIT_1_INIT_GENERIC_MASK);
    return safe_mmio_write32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, tmp);
}

static int pcie_perst_set(unsigned int val)
{
    unsigned int tmp;
    if (safe_mmio_read32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, &tmp) < 0) return -1;
    tmp = (tmp & ~RGR1_SW_INIT_1_PERST_MASK) | (val & RGR1_SW_INIT_1_PERST_MASK);
    return safe_mmio_write32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, tmp);
}

/* ilog2 + inbound-BAR size encoding (Linux brcm_pcie_encode_ibar_size). */
static int pcie_ilog2(unsigned long v) { int n = -1; while (v) { v >>= 1; n++; } return n; }
static unsigned int pcie_encode_ibar(unsigned long size)
{
    int l = pcie_ilog2(size);
    if (l >= 12 && l <= 15) return (unsigned int)((l - 12) + 0x1c);
    if (l >= 16 && l <= 37) return (unsigned int)(l - 15);
    return 0;
}

/* Outbound window 0: CPU `cpu` (40-bit) -> PCIe bus address `pci`, length
 * `size`.  Bit math from Circle / rp1pcie.c set_outbound_win(). */
static void pcie_set_outbound_win(unsigned long cpu, unsigned long pci, unsigned long size)
{
    PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_LO) = (unsigned int)pci;
    PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_HI) = (unsigned int)(pci >> 32);
    unsigned long base_mb  = cpu >> 20;
    unsigned long limit_mb = (cpu + size - 1) >> 20;
    unsigned int bl = PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_BLIM);
    bl &= ~0xfff0u;     bl |= ((unsigned int)base_mb  << 4)  & 0xfff0u;
    bl &= ~0xfff00000u; bl |= ((unsigned int)limit_mb << 20) & 0xfff00000u;
    PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_BLIM) = bl;
    PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_BHI) = (unsigned int)(base_mb  >> 12) & 0xff;
    PCIE_REG(PCIE_MISC_CPU_2_PCIE_WIN0_LHI) = (unsigned int)(limit_mb >> 12) & 0xff;
}

/* Run the FULL BCM2711 PCIe RC bring-up (Circle bcmpciehostbridge.cpp order).
 * Logs each step over UART0 so a serial console shows exactly where it stops.
 * Returns: 0 = link up, -1 = MMIO fault, -2 = link never came up.
 * NOTE: touches ONLY the always-on host-wrapper registers (0x0xxx/0x4xxx/
 * 0x9210); no downstream config/MMIO until DL_ACTIVE (that would hang). */
int xhci_pcie_bring_up(void)
{
    unsigned int tmp;
    uart_puts("[pcie] bring-up start (BCM2711 @0xFD500000)\n");

    /* s0: probe the bridge reg.  safe_mmio catches a synchronous abort; a true
     * AXI hang would lock here (serial shows we never pass s0 -> RC powered down). */
    uart_puts("[pcie] s0: read RGR1_SW_INIT_1 ...\n");
    if (safe_mmio_read32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, &tmp) < 0) {
        uart_puts("[pcie] s0: MMIO FAULT (RC not mapped/clocked)\n"); return -1; }
    uart_puts("[pcie] s0: RGR1="); puts_hex32(tmp); uart_puts("\n");

    /* s1: assert bridge SW_INIT + PERST, hold, release bridge */
    pcie_bridge_sw_init_set(1);
    pcie_perst_set(1);
    delay_ms(1);
    pcie_bridge_sw_init_set(0);
    uart_puts("[pcie] s1: bridge reset pulsed, PERST asserted\n");

    /* s2: clear SerDes IDDQ */
    tmp = PCIE_REG(PCIE_MISC_HARD_DEBUG_OFF);
    tmp &= ~HARD_PCIE_HARD_DEBUG_SERDES_IDDQ;
    PCIE_REG(PCIE_MISC_HARD_DEBUG_OFF) = tmp;
    delay_ms(1);
    uart_puts("[pcie] s2: SerDes IDDQ cleared, rev="); puts_hex32(PCIE_REG(PCIE_MISC_REVISION)); uart_puts("\n");

    /* s3: MISC_CTRL — SCB_ACCESS_EN | CFG_READ_UR_MODE | burst=0 | SCB0=4GB */
    tmp = PCIE_REG(PCIE_MISC_MISC_CTRL_OFF);
    tmp |= MISC_CTRL_SCB_ACCESS_EN | MISC_CTRL_CFG_READ_UR_MODE;
    tmp &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;                    /* 128B on BCM2711 */
    tmp &= ~MISC_CTRL_SCB0_SIZE_MASK;
    tmp |= ((unsigned int)(32 - 15) << 27) & MISC_CTRL_SCB0_SIZE_MASK;  /* 4GB view */
    PCIE_REG(PCIE_MISC_MISC_CTRL_OFF) = tmp;
    uart_puts("[pcie] s3: MISC_CTRL set\n");

    /* s4: inbound RC_BAR2 = host RAM at PCIe addr 0 (Pi4 dma-ranges), 4GB;
     * disable RC_BAR1 / RC_BAR3. */
    PCIE_REG(PCIE_MISC_RC_BAR2_LO) = 0x0u | pcie_encode_ibar(0x100000000UL);
    PCIE_REG(PCIE_MISC_RC_BAR2_HI) = 0x0u;
    PCIE_REG(PCIE_MISC_RC_BAR1_LO) &= ~0x1fu;
    PCIE_REG(PCIE_MISC_RC_BAR3_LO) &= ~0x1fu;
    uart_puts("[pcie] s4: inbound RC_BAR2 set\n");

    /* s5: mask all PCIe interrupts (CLR + MASK_SET) */
    PCIE_REG(PCIE_INTR2_CPU_BASE + 0x08) = 0xffffffffu;
    PCIE_REG(PCIE_INTR2_CPU_BASE + 0x10) = 0xffffffffu;
    uart_puts("[pcie] s5: interrupts masked\n");

    /* s6: clamp link to Gen2 */
    tmp = PCIE_REG(PCIE_CAP_REGS + PCI_EXP_LNKCAP);  tmp = (tmp & ~0xfu) | 2; PCIE_REG(PCIE_CAP_REGS + PCI_EXP_LNKCAP)  = tmp;
    tmp = PCIE_REG(PCIE_CAP_REGS + PCI_EXP_LNKCTL2); tmp = (tmp & ~0xfu) | 2; PCIE_REG(PCIE_CAP_REGS + PCI_EXP_LNKCTL2) = tmp;
    uart_puts("[pcie] s6: Gen2 set\n");

    /* s7: outbound window CPU 0x6_00000000 -> PCIe 0xC0000000, 1GB (Pi4 dtsi) */
    pcie_set_outbound_win(0x600000000UL, 0xC0000000UL, 0x40000000UL);
    uart_puts("[pcie] s7: outbound window set\n");

    /* s8: little-endian BAR2 + CLKREQ debug enable */
    PCIE_REG(PCIE_RC_CFG_VENDOR_SPEC_REG1) &= ~0xcu;
    PCIE_REG(PCIE_MISC_HARD_DEBUG_OFF) |= HARD_PCIE_HARD_DEBUG_CLKREQ_EN;
    uart_puts("[pcie] s8: endian/clkreq set\n");

    /* s9: deassert PERST -> start link training; CEM wants 100ms */
    pcie_perst_set(0);
    uart_puts("[pcie] s9: PERST deasserted, training...\n");
    delay_ms(100);

    /* s10: poll for DL_ACTIVE && PHYLINKUP (~100ms) */
    for (int i = 0; i < 100; i++) {
        tmp = PCIE_REG(PCIE_MISC_PCIE_STATUS_OFF);
        if ((tmp & PCIE_STATUS_DL_ACTIVE) && (tmp & PCIE_STATUS_PHYLINKUP)) {
            uart_puts("[pcie] *** LINK UP *** status="); puts_hex32(tmp); uart_puts("\n");
            return 0;
        }
        delay_ms(1);
    }
    uart_puts("[pcie] link DOWN status="); puts_hex32(PCIE_REG(PCIE_MISC_PCIE_STATUS_OFF)); uart_puts("\n");
    return -2;
}

/* VL805 enumeration — call ONLY after xhci_pcie_bring_up() == 0 (link up).
 * Sets up the RC bridge bus numbers, then reads VL805 (bus1/dev0/fn0) VID/DID
 * via the EXT_CFG window.  Logs over UART.  Returns 0 if VL805 (0x1106:0x3483)
 * is seen, -1 otherwise. */
int xhci_pcie_enum_vl805(void)
{
    uart_puts("[pcie] enum: configuring RC bridge buses\n");
    /* RC bridge config space lives at PCIE_BASE + reg (bus0/dev0/fn0). */
    *(volatile unsigned char  *)(PCIE_BASE + 0x19) = 1;   /* secondary bus   = 1 */
    *(volatile unsigned char  *)(PCIE_BASE + 0x1a) = 1;   /* subordinate bus = 1 */
    *(volatile unsigned short *)(PCIE_BASE + 0x04) = 0x0006; /* MEMORY|MASTER     */

    /* point the EXT_CFG window at bus1/dev0/fn0, then read VID/DID @0x00 */
    PCIE_REG(PCIE_EXT_CFG_INDEX) = (1u << 20);
    (void)PCIE_REG(PCIE_EXT_CFG_INDEX);
    unsigned int viddid = *(volatile unsigned int *)(PCIE_BASE + PCIE_EXT_CFG_DATA + 0x00);
    uart_puts("[pcie] enum: bus1 VID/DID = "); puts_hex32(viddid); uart_puts("\n");

    if (!((viddid & 0xffff) == 0x1106 && (viddid >> 16) == 0x3483)) {
        uart_puts("[pcie] enum: VL805 NOT found\n");
        return -1;
    }
    uart_puts("[pcie] enum: *** VL805 found (1106:3483) ***\n");

    /* M3: program VL805 BAR0 to the outbound-window PCIe base, enable memory
     * decode + bus-master, then read the xHCI capability regs through the
     * window (CPU 0x6_00000000 -> PCIe 0xC0000000 -> VL805 BAR0). */
    {
        volatile unsigned char *cfg = (volatile unsigned char *)(PCIE_BASE + PCIE_EXT_CFG_DATA);
        PCIE_REG(PCIE_EXT_CFG_INDEX) = (1u << 20);              /* bus1/dev0/fn0 */
        (void)PCIE_REG(PCIE_EXT_CFG_INDEX);
        *(volatile unsigned int   *)(cfg + 0x10) = 0xC0000000u | 0x4u;  /* BAR0: 64-bit mem @ PCIe 0xC0000000 */
        *(volatile unsigned int   *)(cfg + 0x14) = 0x00000000u;        /* BAR1: high dword = 0 */
        *(volatile unsigned short *)(cfg + 0x04) = 0x0006;             /* COMMAND: MEMORY | BUS_MASTER */
        __asm__ volatile ("dsb sy" ::: "memory");
        uart_puts("[pcie] enum: VL805 BAR0=0xC0000000, cmd=MEM|MASTER\n");

        volatile unsigned int *xh = (volatile unsigned int *)VL805_XHCI_MMIO;
        unsigned int cap0   = xh[0];                 /* 32-bit: [7:0]=CAPLENGTH [31:16]=HCIVERSION */
        unsigned int caplen = cap0 & 0xff;
        unsigned int hciver = (cap0 >> 16) & 0xffff;
        unsigned int hcs1   = xh[0x04 / 4];          /* HCSPARAMS1 */
        uart_puts("[xhci] CAPLENGTH=");   puts_hex32(caplen);
        uart_puts(" HCIVERSION=");        puts_hex32(hciver);
        uart_puts(" HCSPARAMS1=");        puts_hex32(hcs1);   uart_puts("\n");
        if (hciver == 0x0100 || hciver == 0x0110 || hciver == 0x0120)
            uart_puts("[xhci] *** controller reachable (sane HCIVERSION) ***\n");
        else
            uart_puts("[xhci] WARN: HCIVERSION unexpected (window/BAR not right?)\n");
    }
    return 0;
}

/* ===== M4: VL805 xHCI controller init (ported from the rp1usb.c reference, minus
 * the DWC3-specific quirks — VL805 is a VIA xHCI, not a Synopsys DWC3).  Pi4
 * inbound RC_BAR2 maps PCIe addr 0 -> host RAM 0, so the device-DMA address of
 * a buffer == its physical (== identity-mapped virtual) address: XDA is the
 * identity.  D-cache is off (the boot log says so) so DMA is coherent. ===== */
struct xhci_trb { unsigned int p0, p1, status, control; };
#define XCMD_N        64
#define XEVT_N        64
#define XSCRATCH_MAX  64
static unsigned long long x_dcbaa[256]                       __attribute__((aligned(64)));
static struct xhci_trb    x_cmd[XCMD_N]                      __attribute__((aligned(64)));
static struct xhci_trb    x_evt[XEVT_N]                      __attribute__((aligned(64)));
static unsigned long long x_erst[4]                          __attribute__((aligned(64)));
static unsigned long long x_scratch_arr[XSCRATCH_MAX]        __attribute__((aligned(64)));
static unsigned char      x_scratch_buf[XSCRATCH_MAX][4096]  __attribute__((aligned(4096)));
static unsigned long x_base, x_oper, x_rt, x_db;
static int x_cmd_idx, x_cmd_cycle = 1, x_evt_idx, x_evt_cycle = 1, x_ctx_stride = 32;
static unsigned int x_usbsts_after;
static int x_running;

#define XDA(p)      ((unsigned long)(p))            /* Pi4: PCIe addr == phys */
#define XR(b,o)     (*(volatile unsigned int *)((unsigned long)(b)+(o)))
/* operational regs (from oper = base + CAPLENGTH) */
#define XOP_USBCMD  0x00
#define XOP_USBSTS  0x04
#define XOP_CRCR    0x18
#define XOP_DCBAAP  0x30
#define XOP_CONFIG  0x38
/* capability regs */
#define XCAP_HCSPARAMS1 0x04
#define XCAP_HCSPARAMS2 0x08
#define XCAP_HCCPARAMS1 0x10
#define XCAP_DBOFF      0x14
#define XCAP_RTSOFF     0x18
/* runtime interrupter-0 (rt + offset) */
#define XIR0_IMAN   0x20
#define XIR0_ERSTSZ 0x28
#define XIR0_ERSTBA 0x30
#define XIR0_ERDP   0x38
#define XUSBCMD_RS    (1u<<0)
#define XUSBCMD_HCRST (1u<<1)
#define XUSBSTS_HCH   (1u<<0)
#define XUSBSTS_CNR   (1u<<11)

/* expose the live xHCI register bases to later phases (M5/M6). */
unsigned long xhci_oper_base(void) { return x_oper; }
unsigned long xhci_rt_base(void)   { return x_rt; }
unsigned long xhci_db_base(void)   { return x_db; }
int           xhci_ctx_stride(void){ return x_ctx_stride; }
unsigned int  xhci_usbsts(void)    { return x_usbsts_after; }
int           xhci_is_running(void){ return x_running; }

int xhci_vl805_init(void)
{
    unsigned long base = VL805_XHCI_MMIO;
    x_base = base;
    unsigned int caplen = XR(base, 0) & 0xff;
    x_oper = base + caplen;
    x_rt   = base + (XR(base, XCAP_RTSOFF) & ~0x1Fu);
    x_db   = base + (XR(base, XCAP_DBOFF)  & ~0x3u);
    x_ctx_stride = (XR(base, XCAP_HCCPARAMS1) & (1u<<2)) ? 64 : 32;   /* CSZ */
    uart_puts("[xhci] init: oper/rt/db resolved, ctx_stride=");
    puts_hex32((unsigned)x_ctx_stride); uart_puts("\n");

    /* 0. The VL805 may still be booting its own firmware after the PERST
     * deassert — wait for CNR (Controller Not Ready) to clear first. */
    { int ms=0; while ((XR(x_oper,XOP_USBSTS)&XUSBSTS_CNR) && ms<3000){ delay_ms(5); ms+=5; }
      uart_puts("[xhci] init: pre-reset CNR ");
      uart_puts((XR(x_oper,XOP_USBSTS)&XUSBSTS_CNR) ? "STILL SET after 3s\n" : "clear\n"); }

    /* 1. Stop, then reset. */
    XR(x_oper, XOP_USBCMD) &= ~XUSBCMD_RS;
    { int i=0; while (!(XR(x_oper,XOP_USBSTS)&XUSBSTS_HCH) && i<200){ delay_ms(1); i++; } }
    XR(x_oper, XOP_USBCMD) |= XUSBCMD_HCRST;
    { int i=0; while ((XR(x_oper,XOP_USBCMD)&XUSBCMD_HCRST) && i<1000){ delay_ms(1); i++; }
      uart_puts("[xhci] init: HCRST cleared after "); puts_hex32((unsigned)i); uart_puts("ms\n"); }
    { int i=0; while ((XR(x_oper,XOP_USBSTS)&XUSBSTS_CNR) && i<3000){ delay_ms(1); i++; }
      uart_puts("[xhci] init: post-reset CNR ");
      uart_puts((XR(x_oper,XOP_USBSTS)&XUSBSTS_CNR) ? "STILL SET (TIMEOUT)\n" : "clear\n"); }

    /* 2. DCBAA + scratchpad. */
    for (int i=0;i<256;i++) x_dcbaa[i]=0;
    unsigned int hcs2 = XR(base, XCAP_HCSPARAMS2);
    int nscratch = (int)(((hcs2>>27)&0x1f) | (((hcs2>>21)&0x1f)<<5));
    if (nscratch > XSCRATCH_MAX) nscratch = XSCRATCH_MAX;
    for (int i=0;i<nscratch;i++) x_scratch_arr[i] = XDA(&x_scratch_buf[i][0]);
    if (nscratch > 0) x_dcbaa[0] = XDA(&x_scratch_arr[0]);
    XR(x_oper, XOP_DCBAAP)   = (unsigned int)(XDA(x_dcbaa) & 0xffffffff);
    XR(x_oper, XOP_DCBAAP+4) = (unsigned int)(XDA(x_dcbaa) >> 32);
    uart_puts("[xhci] init: DCBAA set, scratch="); puts_hex32((unsigned)nscratch); uart_puts("\n");

    /* 3. Command ring + link TRB. */
    for (int i=0;i<XCMD_N;i++){ x_cmd[i].p0=x_cmd[i].p1=x_cmd[i].status=x_cmd[i].control=0; }
    x_cmd[XCMD_N-1].p0 = (unsigned int)(XDA(x_cmd)&0xffffffff);
    x_cmd[XCMD_N-1].p1 = (unsigned int)(XDA(x_cmd)>>32);
    x_cmd[XCMD_N-1].control = (6u<<10)|(1u<<1)|1u;       /* Link TRB, TC=1, cycle */
    x_cmd_idx=0; x_cmd_cycle=1;
    XR(x_oper, XOP_CRCR)   = (unsigned int)((XDA(x_cmd)&0xffffffff) | 1u);  /* RCS=1 */
    XR(x_oper, XOP_CRCR+4) = (unsigned int)(XDA(x_cmd)>>32);

    /* 4. Event ring + ERST (interrupter 0). */
    for (int i=0;i<XEVT_N;i++){ x_evt[i].p0=x_evt[i].p1=x_evt[i].status=x_evt[i].control=0; }
    x_evt_idx=0; x_evt_cycle=1;
    x_erst[0] = XDA(x_evt);
    x_erst[1] = (unsigned long long)XEVT_N;
    XR(x_rt, XIR0_ERSTSZ) = 1;
    XR(x_rt, XIR0_ERDP)   = (unsigned int)(XDA(x_evt)&0xffffffff);
    XR(x_rt, XIR0_ERDP+4) = (unsigned int)(XDA(x_evt)>>32);
    XR(x_rt, XIR0_ERSTBA) = (unsigned int)(XDA(x_erst)&0xffffffff);
    XR(x_rt, XIR0_ERSTBA+4)=(unsigned int)(XDA(x_erst)>>32);
    XR(x_rt, XIR0_IMAN)   = 0x2;                          /* IE=0, clear IP */
    uart_puts("[xhci] init: cmd+evt rings set\n");

    /* 5. CONFIG: MaxSlotsEn = HCSPARAMS1.MaxSlots. */
    XR(x_oper, XOP_CONFIG) = (XR(base, XCAP_HCSPARAMS1) & 0xff);

    /* 6. Run. */
    __asm__ volatile ("dsb sy" ::: "memory");
    XR(x_oper, XOP_USBCMD) |= XUSBCMD_RS;
    for (int i=0;i<200 && (XR(x_oper,XOP_USBSTS)&XUSBSTS_HCH);i++) delay_ms(1);

    x_usbsts_after = XR(x_oper, XOP_USBSTS);
    x_running = !(x_usbsts_after & XUSBSTS_HCH);
    uart_puts("[xhci] init: USBSTS="); puts_hex32(x_usbsts_after);
    uart_puts(x_running ? " *** RUNNING ***\n" : " halted\n");
    return x_running ? 0 : -1;
}

/* ===== M5: port reset + Enable Slot + Address Device + GET_DESCRIPTOR =====
 * Ported from the rp1usb.c reference (single-controller, mouse-focused). ===== */
static int x_cmd_push(unsigned int p0, unsigned int p1, unsigned int status, unsigned int control)
{
    struct xhci_trb *t = &x_cmd[x_cmd_idx];
    t->p0=p0; t->p1=p1; t->status=status;
    t->control = (control & ~1u) | (x_cmd_cycle & 1u);
    __asm__ volatile ("dsb sy":::"memory");
    x_cmd_idx++;
    if (x_cmd_idx == XCMD_N-1) {
        x_cmd[XCMD_N-1].control = (x_cmd[XCMD_N-1].control & ~1u) | (x_cmd_cycle & 1u);
        __asm__ volatile ("dsb sy":::"memory");
        x_cmd_idx=0; x_cmd_cycle ^= 1;
    }
    XR(x_db, 0) = 0;                 /* command-ring doorbell */
    __asm__ volatile ("dsb sy":::"memory");
    return 0;
}
static struct xhci_trb x_event_wait(unsigned int want_type)
{
    struct xhci_trb ev; ev.p0=ev.p1=ev.status=ev.control=0;
    for (int spin=0; spin<500; spin++) {
        struct xhci_trb *e = &x_evt[x_evt_idx];
        __asm__ volatile ("dsb sy":::"memory");
        if ((e->control & 1u) == (unsigned)x_evt_cycle) {
            ev = *e;
            x_evt_idx++;
            if (x_evt_idx == XEVT_N) { x_evt_idx=0; x_evt_cycle ^= 1; }
            unsigned long erdp = XDA(&x_evt[x_evt_idx]);
            XR(x_rt, XIR0_ERDP)   = (unsigned)((erdp & 0xffffffff) | (1u<<3));
            XR(x_rt, XIR0_ERDP+4) = (unsigned)(erdp >> 32);
            unsigned int type = (ev.control>>10) & 0x3f;
            if (want_type==0 || type==want_type) return ev;
            spin=0; continue;
        }
        delay_ms(1);
    }
    return ev;
}

/* device contexts + EP0 transfer ring (single device for now) */
static unsigned char x_input_ctx[33*64] __attribute__((aligned(64)));
static unsigned char x_dev_ctx[33*64]   __attribute__((aligned(64)));
static struct xhci_trb x_ep0_ring[16]   __attribute__((aligned(64)));
static int x_ep0_idx, x_ep0_cycle = 1;
static unsigned char x_xfer[256] __attribute__((aligned(64)));
static unsigned int x_enum_portsc, x_enum_slot, x_enum_cc, x_addr_cc, x_desc_cc, x_desc_len;
static unsigned int *xctx(unsigned char *p, int idx){ return (unsigned int *)(p + idx*x_ctx_stride); }

/* Reset 1-based port `p` and Enable Slot.  Returns slot id, or -1. */
int xhci_enum_slot(int p)
{
    unsigned long psc = x_oper + 0x400 + (p-1)*0x10;
    XR(psc,0) = (1u<<9)|(1u<<4);                 /* PP + PR */
    for (int i=0;i<100;i++){ delay_ms(1); if (!(XR(psc,0)&(1u<<4))) break; }
    delay_ms(30);
    x_enum_portsc = XR(psc,0);
    for (int i=0;i<20;i++){ unsigned v=XR(psc,0); if ((v&1u)&&((v>>10)&0xf)){ x_enum_portsc=v; break; } x_enum_portsc=v; delay_ms(3); }
    if (!(x_enum_portsc & 1u)) return -1;        /* nothing connected */
    x_cmd_push(0,0,0,(9u<<10));                  /* Enable Slot */
    struct xhci_trb ev = x_event_wait(33);
    x_enum_cc   = (ev.status>>24)&0xff;
    x_enum_slot = (ev.control>>24)&0xff;
    uart_puts("[xhci] port"); uart_putc((char)('0'+p));
    uart_puts(" PORTSC="); puts_hex32(x_enum_portsc);
    uart_puts(" enable-slot cc="); puts_hex32(x_enum_cc);
    uart_puts(" slot="); puts_hex32(x_enum_slot); uart_puts("\n");
    return (x_enum_cc==1) ? (int)x_enum_slot : -1;
}

int xhci_address_device(int slot, int port, int speed)
{
    for (int i=0;i<16;i++){ x_ep0_ring[i].p0=x_ep0_ring[i].p1=x_ep0_ring[i].status=x_ep0_ring[i].control=0; }
    x_ep0_ring[15].p0=(unsigned)(XDA(x_ep0_ring)&0xffffffff);
    x_ep0_ring[15].p1=(unsigned)(XDA(x_ep0_ring)>>32);
    x_ep0_ring[15].control=(6u<<10)|(1u<<1)|1u;
    x_ep0_idx=0; x_ep0_cycle=1;

    for (unsigned i=0;i<sizeof x_input_ctx;i++) x_input_ctx[i]=0;
    for (unsigned i=0;i<33*64;i++)              x_dev_ctx[i]=0;
    unsigned int *icc=xctx(x_input_ctx,0); icc[1]=(1u<<0)|(1u<<1);     /* add slot + EP0 */
    unsigned int *sc =xctx(x_input_ctx,1);
    sc[0]=(1u<<27)|((unsigned)speed<<20);
    sc[1]=((unsigned)port&0xff)<<16;
    int mps=(speed==4)?512:(speed==3)?64:8;
    unsigned int *ep0=xctx(x_input_ctx,2);
    ep0[1]=(4u<<3)|(3u<<1)|((unsigned)mps<<16);
    unsigned long trd=XDA(x_ep0_ring)|1u;
    ep0[2]=(unsigned)(trd&0xffffffff); ep0[3]=(unsigned)(trd>>32); ep0[4]=8;
    x_dcbaa[slot]=XDA(x_dev_ctx);
    __asm__ volatile ("dsb sy":::"memory");
    unsigned long ic=XDA(x_input_ctx);
    x_cmd_push((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(11u<<10)|((unsigned)slot<<24));
    struct xhci_trb ev=x_event_wait(33);
    x_addr_cc=(ev.status>>24)&0xff;
    uart_puts("[xhci] address-device cc="); puts_hex32(x_addr_cc); uart_puts("\n");
    return (x_addr_cc==1)?0:-1;
}

static void x_ep0_push(unsigned int p0,unsigned int p1,unsigned int status,unsigned int control)
{
    struct xhci_trb *t=&x_ep0_ring[x_ep0_idx];
    t->p0=p0;t->p1=p1;t->status=status;
    t->control=(control & ~1u)|(x_ep0_cycle&1u);
    __asm__ volatile ("dsb sy":::"memory");
    x_ep0_idx++;
    if (x_ep0_idx==15){ x_ep0_ring[15].control=(x_ep0_ring[15].control&~1u)|(x_ep0_cycle&1u); __asm__ volatile("dsb sy":::"memory"); x_ep0_idx=0; x_ep0_cycle^=1; }
}
int xhci_get_descriptor(int slot, int dtype, int dindex, int len)
{
    if (len > (int)sizeof x_xfer) len = sizeof x_xfer;
    for (int i=0;i<len;i++) x_xfer[i]=0;
    unsigned int wValue=((unsigned)dtype<<8)|(unsigned)dindex;
    x_ep0_push(0x80u|(6u<<8)|(wValue<<16),((unsigned)len<<16),8,(2u<<10)|(1u<<6)|(3u<<16));
    unsigned long ba=XDA(x_xfer);
    x_ep0_push((unsigned)(ba&0xffffffff),(unsigned)(ba>>32),(unsigned)len,(3u<<10)|(1u<<16));
    x_ep0_push(0,0,0,(4u<<10)|(1u<<5));
    XR(x_db, slot*4)=1;
    __asm__ volatile ("dsb sy":::"memory");
    struct xhci_trb ev=x_event_wait(32);
    x_desc_cc=(ev.status>>24)&0xff;
    x_desc_len=(unsigned)len-(ev.status&0xffffff);
    return (x_desc_cc==1)?(int)x_desc_len:-1;
}
unsigned int xhci_xfer_byte(int i){ return (i>=0&&i<256)?x_xfer[i]:0; }

/* M5 driver: scan ports 1..MaxPorts, for each connected port reset+enable slot
 * +address device + read the 18-byte device descriptor (VID/PID).  Logs all. */
int xhci_vl805_enum_mouse(void)
{
    if (!x_running) { uart_puts("[xhci] enum: controller not running\n"); return -1; }
    int nports = (int)((XR(x_base, XCAP_HCSPARAMS1) >> 24) & 0xff);
    uart_puts("[xhci] enum: scanning "); puts_hex32((unsigned)nports); uart_puts(" ports\n");
    for (int p=1; p<=nports && p<=5; p++) {
        int slot = xhci_enum_slot(p);
        if (slot < 0) continue;
        int speed = (int)((x_enum_portsc>>10)&0xf);
        if (xhci_address_device(slot, p, speed) != 0) continue;
        int got = xhci_get_descriptor(slot, 1, 0, 18);    /* DEVICE descriptor */
        if (got >= 12) {
            unsigned vid = x_xfer[8] | (x_xfer[9]<<8);
            unsigned pid = x_xfer[10] | (x_xfer[11]<<8);
            unsigned cls = x_xfer[4];
            uart_puts("[xhci] *** device on port"); uart_putc((char)('0'+p));
            uart_puts(": VID="); puts_hex32(vid);
            uart_puts(" PID="); puts_hex32(pid);
            uart_puts(" class="); puts_hex32(cls); uart_puts(" ***\n");
        } else {
            uart_puts("[xhci] enum: get-descriptor failed cc="); puts_hex32(x_desc_cc); uart_puts("\n");
        }
    }
    uart_puts("[xhci] enum: scan done\n");
    return 0;
}

/* ===== M5.5: USB hub enumeration + TT (the VL805 root ports are internal
 * hubs; the mouse/keyboard sit downstream).  Per-slot EP0 ring + output ctx so
 * a hub and its child can be addressed concurrently. ===== */
#define XMAXSLOT 8
static unsigned char   x_devctx_s[XMAXSLOT][33*64] __attribute__((aligned(64)));
static struct xhci_trb x_ep0_s[XMAXSLOT][16]       __attribute__((aligned(64)));
static int             x_ep0idx_s[XMAXSLOT], x_ep0cyc_s[XMAXSLOT];
static unsigned char   x_hbuf[256] __attribute__((aligned(64)));   /* hub ctrl-IN buf */

static void xs_ep0_push(int slot, unsigned int p0, unsigned int p1, unsigned int status, unsigned int control)
{
    struct xhci_trb *r = x_ep0_s[slot];
    struct xhci_trb *t = &r[x_ep0idx_s[slot]];
    t->p0=p0;t->p1=p1;t->status=status;
    t->control=(control & ~1u)|(x_ep0cyc_s[slot]&1u);
    __asm__ volatile("dsb sy":::"memory");
    x_ep0idx_s[slot]++;
    if (x_ep0idx_s[slot]==15){ r[15].control=(r[15].control&~1u)|(x_ep0cyc_s[slot]&1u); __asm__ volatile("dsb sy":::"memory"); x_ep0idx_s[slot]=0; x_ep0cyc_s[slot]^=1; }
}
/* control transfer with a data-IN stage into x_hbuf; returns bytes or -1. */
static int xs_control_in(int slot, unsigned bmReq, unsigned bReq, unsigned wValue, unsigned wIndex, int len)
{
    if (len > (int)sizeof x_hbuf) len = sizeof x_hbuf;
    for (int i=0;i<len;i++) x_hbuf[i]=0;
    xs_ep0_push(slot, bmReq|(bReq<<8)|(wValue<<16), (wIndex)|((unsigned)len<<16), 8, (2u<<10)|(1u<<6)|(3u<<16));
    unsigned long ba=XDA(x_hbuf);
    xs_ep0_push(slot, (unsigned)(ba&0xffffffff),(unsigned)(ba>>32),(unsigned)len,(3u<<10)|(1u<<16));
    xs_ep0_push(slot, 0,0,0,(4u<<10)|(1u<<5));
    XR(x_db, slot*4)=1; __asm__ volatile("dsb sy":::"memory");
    struct xhci_trb ev=x_event_wait(32);
    unsigned cc=(ev.status>>24)&0xff;
    return (cc==1||cc==13)?(int)((unsigned)len-(ev.status&0xffffff)):-1;
}
static int xs_control_nodata(int slot, unsigned bmReq, unsigned bReq, unsigned wValue, unsigned wIndex)
{
    xs_ep0_push(slot, bmReq|(bReq<<8)|(wValue<<16), wIndex, 8, (2u<<10)|(1u<<6)|(0u<<16));
    xs_ep0_push(slot, 0,0,0,(4u<<10)|(1u<<16)|(1u<<5));
    XR(x_db, slot*4)=1; __asm__ volatile("dsb sy":::"memory");
    struct xhci_trb ev=x_event_wait(32);
    return ((ev.status>>24)&0xff)==1?0:-1;
}

/* Address a device.  route=0 + tt_hub=0 for a root-port device; for a device on
 * downstream port `dport` of hub `tt_hub` (slot id), route=dport and (for LS/FS)
 * tt_hub/tt_port carry the TT.  root_port = the VL805 root port the chain hangs
 * off.  Builds a per-slot EP0 ring + output ctx. */
static int xs_address(int slot, int root_port, unsigned route, int speed, int tt_hub, int tt_port)
{
    struct xhci_trb *r = x_ep0_s[slot];
    for (int i=0;i<16;i++){ r[i].p0=r[i].p1=r[i].status=r[i].control=0; }
    r[15].p0=(unsigned)(XDA(r)&0xffffffff); r[15].p1=(unsigned)(XDA(r)>>32);
    r[15].control=(6u<<10)|(1u<<1)|1u;
    x_ep0idx_s[slot]=0; x_ep0cyc_s[slot]=1;

    unsigned char *dctx = x_devctx_s[slot];
    for (unsigned i=0;i<sizeof x_input_ctx;i++) x_input_ctx[i]=0;
    for (unsigned i=0;i<33*64;i++)              dctx[i]=0;
    unsigned int *icc=xctx(x_input_ctx,0); icc[1]=(1u<<0)|(1u<<1);
    unsigned int *sc=xctx(x_input_ctx,1);
    sc[0]=(1u<<27)|((unsigned)speed<<20)|(route&0xfffff);       /* entries=1, speed, route */
    sc[1]=((unsigned)root_port&0xff)<<16;                      /* root hub port */
    if (tt_hub){ sc[2]=((unsigned)tt_hub&0xff)|(((unsigned)tt_port&0xff)<<8); } /* TT hub slot + port */
    int mps=(speed==4)?512:(speed==3)?64:8;
    unsigned int *ep0=xctx(x_input_ctx,2);
    ep0[1]=(4u<<3)|(3u<<1)|((unsigned)mps<<16);
    unsigned long trd=XDA(r)|1u;
    ep0[2]=(unsigned)(trd&0xffffffff); ep0[3]=(unsigned)(trd>>32); ep0[4]=8;
    x_dcbaa[slot]=XDA(dctx);
    __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(x_input_ctx);
    x_cmd_push((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(11u<<10)|((unsigned)slot<<24));
    struct xhci_trb ev=x_event_wait(33);
    unsigned cc=(ev.status>>24)&0xff;
    return (cc==1)?0:-(int)cc;
}

/* Get the 18-byte device descriptor of `slot` into x_hbuf; logs VID/PID/class. */
static int xs_dev_desc(int slot, unsigned *vid, unsigned *pid, unsigned *cls)
{
    int got = xs_control_in(slot, 0x80, 6, (1u<<8), 0, 18);
    if (got < 12) return -1;
    *vid=x_hbuf[8]|(x_hbuf[9]<<8); *pid=x_hbuf[10]|(x_hbuf[11]<<8); *cls=x_hbuf[4];
    return 0;
}

/* Mark slot `slot` as a USB hub in its context (Hub=1, NumPorts, MTT/TTT) via a
 * Configure-Endpoint with only the slot context updated. */
static int xs_make_hub(int slot, int nports, int speed)
{
    unsigned char *dctx = x_devctx_s[slot];
    for (unsigned i=0;i<sizeof x_input_ctx;i++) x_input_ctx[i]=0;
    unsigned int *icc=xctx(x_input_ctx,0); icc[1]=(1u<<0);          /* add slot ctx only */
    /* copy current slot ctx from output, then OR in hub fields */
    unsigned int *src=xctx(dctx,1), *sc=xctx(x_input_ctx,1);
    sc[0]=src[0] | (1u<<26);                                       /* Hub = 1 */
    sc[1]=src[1] | (((unsigned)nports&0xff)<<24);                  /* NumberOfPorts */
    sc[2]=src[2] | (speed==3 ? (1u<<25) : 0u);                     /* MTT for HS hub (best-effort) */
    sc[3]=src[3];
    x_dcbaa[slot]=XDA(dctx); __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(x_input_ctx);
    x_cmd_push((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24));
    struct xhci_trb ev=x_event_wait(33);
    return ((ev.status>>24)&0xff)==1?0:-1;
}

/* Hub class requests (wIndex = downstream port, 1-based). */
#define HUB_SET_PORT_FEATURE  (0)   /* bmReq 0x23, bReq 3 */
#define HUB_FEAT_PORT_POWER   8
#define HUB_FEAT_PORT_RESET   4
static int xs_hub_set_port(int slot, int port, int feat){ return xs_control_nodata(slot, 0x23, 3, (unsigned)feat, (unsigned)port); }
static unsigned xs_hub_port_status(int slot, int port)
{
    if (xs_control_in(slot, 0xA3, 0, 0, (unsigned)port, 4) < 4) return 0;
    return x_hbuf[0] | (x_hbuf[1]<<8) | (x_hbuf[2]<<16) | (x_hbuf[3]<<24);
}

/* Enumerate one USB hub at `hub_slot` (already addressed) on VL805 root port
 * `root_port`: configure it, power+scan its downstream ports, and for each
 * connected port reset + Enable Slot + Address (routed, with TT) + dev-desc.
 * Reports each downstream device's VID/PID/class.  Returns devices found. */
static int x_next_slot = 3;     /* hub used slots 1,2; children start at 3 */
int xhci_hid_setup(int slot, int root_port, unsigned route, int speed, int tt_hub, int tt_port);  /* M6, below */
int xhci_msd_setup(int slot, int root_port, unsigned route, int speed, int tt_hub, int tt_port);  /* MSD, below */

/* Enumeration journal — a compact, HTTP-queryable record of what each USB port
 * enumerated to (port/slot/speed/class + driver decision), since the boot UART
 * log scrolls off the HDMI desktop.  Surfaced by /usbdiag via xhci_enum_journal. */
static char x_enum_j[768];
static int  x_enum_jl = 0;
static void x_jlog(const char *s) { while (*s && x_enum_jl < (int)sizeof x_enum_j - 1) x_enum_j[x_enum_jl++] = *s++; x_enum_j[x_enum_jl]=0; }
static void x_jhex(unsigned v) { char b[9]; for(int i=0;i<2;i++){unsigned n=(v>>((1-i)*4))&0xF; b[i]=(char)(n<10?'0'+n:'a'+n-10);} b[2]=0; x_jlog(b); }
int xhci_enum_journal(char *out, int max) { int i=0; for(; i<x_enum_jl && i<max-1; i++) out[i]=x_enum_j[i]; out[i]=0; return i; }

/* Read the config descriptor and return the first interface's bInterfaceClass
 * (8 = Mass Storage, 3 = HID, 9 = hub).  Used to route enumeration to the right
 * driver.  Returns -1 if no config / interface descriptor is found. */
static int xs_iface_class(int slot)
{
    int got = xs_control_in(slot, 0x80, 6, (2u<<8), 0, 9);
    if (got < 4 || x_hbuf[1] != 2) return -1;
    int wtot = x_hbuf[2] | (x_hbuf[3]<<8);
    if (wtot > (int)sizeof x_hbuf) wtot = sizeof x_hbuf;
    xs_control_in(slot, 0x80, 6, (2u<<8), 0, wtot);
    int pos = 0;
    while (pos + 2 <= wtot) {
        int blen = x_hbuf[pos], btype = x_hbuf[pos+1];
        if (blen < 2) break;
        if (btype == 4) return x_hbuf[pos+5];       /* bInterfaceClass */
        pos += blen;
    }
    return -1;
}

int xhci_hub_enumerate(int hub_slot, int root_port, int hub_speed)
{
    uart_puts("[xhci] hub: configuring slot="); puts_hex32((unsigned)hub_slot); uart_puts("\n");
    if (xs_control_nodata(hub_slot, 0x00, 9, 1, 0) != 0)           /* SET_CONFIGURATION(1) */
        uart_puts("[xhci] hub: SET_CONFIG failed (continuing)\n");
    /* hub descriptor: bmReq 0xA0, GET_DESCRIPTOR, wValue 0x29<<8 (hub) */
    int hd = xs_control_in(hub_slot, 0xA0, 6, (0x29u<<8), 0, 9);
    int nports = (hd >= 3) ? x_hbuf[2] : 0;
    uart_puts("[xhci] hub: nports="); puts_hex32((unsigned)nports); uart_puts("\n");
    if (nports <= 0 || nports > 15) { uart_puts("[xhci] hub: bad nports\n"); return 0; }
    xs_make_hub(hub_slot, nports, hub_speed);

    int found = 0;
    for (int dp=1; dp<=nports; dp++) {
        xs_hub_set_port(hub_slot, dp, HUB_FEAT_PORT_POWER);
        delay_ms(20);
    }
    delay_ms(120);                                                /* PWRON2PWRGOOD */
    for (int dp=1; dp<=nports; dp++) {
        unsigned st = xs_hub_port_status(hub_slot, dp);
        if (!(st & 0x1)) continue;                                /* PORT_CONNECTION */
        uart_puts("[xhci] hub port"); uart_putc((char)('0'+dp));
        uart_puts(" connected, status="); puts_hex32(st); uart_puts("\n");
        xs_hub_set_port(hub_slot, dp, HUB_FEAT_PORT_RESET);
        for (int i=0;i<20;i++){ delay_ms(10); st=xs_hub_port_status(hub_slot, dp); if (st & (1u<<4)) break; } /* PORT_ENABLE */
        delay_ms(20);
        st = xs_hub_port_status(hub_slot, dp);
        /* USB2 hub port status: bit9=low-speed, bit10=high-speed (else full). */
        int speed = (st & (1u<<10)) ? 3 /*HS*/ : (st & (1u<<9)) ? 2 /*LS*/ : 1 /*FS*/;
        if (x_next_slot >= XMAXSLOT) break;
        x_cmd_push(0,0,0,(9u<<10));                               /* Enable Slot */
        struct xhci_trb ev=x_event_wait(33);
        if (((ev.status>>24)&0xff)!=1) { uart_puts("[xhci] hub: enable-slot failed\n"); continue; }
        int cslot=(ev.control>>24)&0xff;
        /* TT: LS/FS device behind a HS hub needs the hub's slot+port as TT. */
        int tt_hub = (speed!=3) ? hub_slot : 0;
        int rc = xs_address(cslot, root_port, (unsigned)dp, speed, tt_hub, dp);
        if (rc != 0) { uart_puts("[xhci] hub: address-device failed cc="); puts_hex32((unsigned)(-rc)); uart_puts("\n"); continue; }
        unsigned vid=0,pid=0,cls=0;
        if (xs_dev_desc(cslot,&vid,&pid,&cls)==0) {
            uart_puts("[xhci] *** HID? dev hubport"); uart_putc((char)('0'+dp));
            uart_puts(" slot="); puts_hex32((unsigned)cslot);
            uart_puts(" speed="); puts_hex32((unsigned)speed);
            uart_puts(" VID="); puts_hex32(vid);
            uart_puts(" PID="); puts_hex32(pid);
            uart_puts(" class="); puts_hex32(cls); uart_puts(" ***\n");
            found++;
            /* Route by interface class: 8 = USB Mass Storage (Bulk-Only) -> /sd,
             * everything else -> HID interrupt endpoint (mouse/keyboard). */
            int icls = xs_iface_class(cslot);
            x_jlog("[h"); x_jhex((unsigned)dp); x_jlog("s"); x_jhex((unsigned)cslot);
            x_jlog("sp"); x_jhex((unsigned)speed); x_jlog("ic"); x_jhex((unsigned)(icls&0xff));
            if (icls == 8) {
                x_jlog("=MSD] ");
                uart_puts("[xhci] *** Mass Storage device on hubport");
                uart_putc((char)('0'+dp)); uart_puts(" -> /sd ***\n");
                xhci_msd_setup(cslot, root_port, (unsigned)dp, speed, tt_hub, dp);
            } else {
                x_jlog("=HID] ");
                /* M6: bring up its HID interrupt endpoint (route=dp, TT if LS/FS). */
                xhci_hid_setup(cslot, root_port, (unsigned)dp, speed, tt_hub, dp);
            }
        } else {
            x_jlog("[h"); x_jhex((unsigned)dp); x_jlog("(desc-fail)] ");
        }
    }
    uart_puts("[xhci] hub: enumerate done, found="); puts_hex32((unsigned)found); uart_puts("\n");
    return found;
}

/* Top-level: scan VL805 root ports, address each (already done in enum_mouse but
 * redone here standalone), and for any that is a hub (class 9) recurse into it. */
int xhci_vl805_enum_full(void)
{
    if (!x_running) { uart_puts("[xhci] full: not running\n"); return -1; }
    int nports=(int)((XR(x_base,XCAP_HCSPARAMS1)>>24)&0xff);
    x_enum_jl = 0; x_jlog("ports="); x_jhex((unsigned)nports); x_jlog(" ");
    x_next_slot = 1;
    for (int p=1;p<=nports && p<=5;p++) {
        unsigned long psc=x_oper+0x400+(p-1)*0x10;
        XR(psc,0)=(1u<<9)|(1u<<4);
        for (int i=0;i<100;i++){ delay_ms(1); if(!(XR(psc,0)&(1u<<4))) break; }
        delay_ms(30);
        unsigned v=XR(psc,0);
        if (!(v&1u)) continue;
        int speed=(int)((v>>10)&0xf);
        x_jlog("rp"); x_jhex((unsigned)p); x_jlog("sp"); x_jhex((unsigned)speed);
        if (x_next_slot>=XMAXSLOT) break;
        x_cmd_push(0,0,0,(9u<<10));
        struct xhci_trb ev=x_event_wait(33);
        if (((ev.status>>24)&0xff)!=1) { x_jlog("(en-fail) "); continue; }
        int slot=(ev.control>>24)&0xff;
        if (slot >= x_next_slot) x_next_slot = slot+1;
        if (xs_address(slot, p, 0, speed, 0, 0)!=0) { x_jlog("(addr-fail) "); continue; }
        unsigned vid=0,pid=0,cls=0;
        if (xs_dev_desc(slot,&vid,&pid,&cls)!=0) { x_jlog("(desc-fail) "); continue; }
        x_jlog("cls"); x_jhex(cls); x_jlog(" ");
        uart_puts("[xhci] root port"); uart_putc((char)('0'+p));
        uart_puts(" slot="); puts_hex32((unsigned)slot);
        uart_puts(" VID="); puts_hex32(vid); uart_puts(" PID="); puts_hex32(pid);
        uart_puts(" class="); puts_hex32(cls); uart_puts("\n");
        /* A root-port device is usually a VL805 internal hub (class 9, or the
         * Genesys USB2 hub which reports device class 0), but a USB-3.0 thumb
         * drive in a blue port enumerates DIRECTLY on a root port at SuperSpeed
         * (class 0, interface class 8).  Disambiguate by interface class. */
        int icls = xs_iface_class(slot);
        x_jlog("ic"); x_jhex((unsigned)(icls & 0xff)); x_jlog(" ");
        if (icls == 8) {
            x_jlog("=MSDdirect ");
            uart_puts("[xhci] *** Mass Storage device direct on root port");
            uart_putc((char)('0'+p)); uart_puts(" -> /sd ***\n");
            xhci_msd_setup(slot, p, 0, speed, 0, 0);
        } else if (cls == 9 || cls == 0) {
            xhci_hub_enumerate(slot, p, speed);
        }
    }
    return 0;
}

/* ===== M6: HID setup (config-desc walk -> interrupt-IN EP -> Configure
 * Endpoint with route/TT -> SET_PROTOCOL boot) + interrupt pump -> cursor. == */
static struct xhci_trb x_int_ring[XMAXSLOT][16] __attribute__((aligned(64)));
static int             x_int_idx[XMAXSLOT], x_int_cyc[XMAXSLOT];
static unsigned char   x_hid_buf[XMAXSLOT][8]   __attribute__((aligned(64)));
static int x_mouse_slot=-1, x_mouse_dci, x_mouse_iface, x_kbd_slot=-1, x_kbd_dci, x_kbd_iface;
static unsigned char x_kbd_prev[8];
static unsigned x_mouse_prev_btn;   /* prev mouse button bits, for break edge-detect */

/* Keyboard auto-repeat.  A HID boot keyboard reports only on key state change,
 * so a held key arrives once; we synthesise repeats on the periodic pump.
 * Timebase = timer_ticks() at TIMER_HZ=100 (1 tick = 10 ms). */
#define KBD_RPT_DELAY 40            /* 400 ms before the first auto-repeat */
#define KBD_RPT_RATE   4            /*  40 ms between repeats (~25 cps)    */
static unsigned      x_kbd_rk;             /* HID usage being repeated (0 = none) */
static int           x_kbd_rk_shift, x_kbd_rk_ctrl;
static unsigned long x_kbd_rk_next;        /* timer_ticks() value for next repeat */
static unsigned long x_mouse_reports, x_kbd_reports;
static volatile int  x_ctrl_c_pending; /* sticky: set when a report shows Ctrl+'c' */
static int x_poll_mode = 0;   /* 0 = interrupt EP (drain event ring); 1 = EP0 GET_REPORT poll */
static unsigned x_xfer_events, x_last_xfer_cc, x_last_xfer_slot, x_last_xfer_dci;

static void x_hid_arm(int slot, int dci)
{
    struct xhci_trb *r=x_int_ring[slot];
    struct xhci_trb *t=&r[x_int_idx[slot]];
    unsigned long ba=XDA(x_hid_buf[slot]);
    t->p0=(unsigned)(ba&0xffffffff); t->p1=(unsigned)(ba>>32);
    t->status=8;
    t->control=(1u<<10)|(1u<<5)|(x_int_cyc[slot]&1u);          /* Normal + IOC */
    __asm__ volatile("dsb sy":::"memory");
    x_int_idx[slot]++;
    if (x_int_idx[slot]==15){ r[15].control=(r[15].control&~1u)|(x_int_cyc[slot]&1u); __asm__ volatile("dsb sy":::"memory"); x_int_idx[slot]=0; x_int_cyc[slot]^=1; }
    XR(x_db, slot*4)=(unsigned)dci;
    __asm__ volatile("dsb sy":::"memory");
}

/* Configure a HID device's interrupt-IN endpoint and start it.  Re-supplies the
 * slot ctx (route/speed/root_port/TT) so Configure Endpoint preserves the path. */
int xhci_hid_setup(int slot, int root_port, unsigned route, int speed, int tt_hub, int tt_port)
{
    /* config descriptor: header then full. */
    int got = xs_control_in(slot, 0x80, 6, (2u<<8), 0, 9);
    if (got < 4 || x_hbuf[1] != 2) { uart_puts("[xhci] hid: no config desc\n"); return -1; }
    int wtot = x_hbuf[2] | (x_hbuf[3]<<8);
    if (wtot > (int)sizeof x_hbuf) wtot = sizeof x_hbuf;
    xs_control_in(slot, 0x80, 6, (2u<<8), 0, wtot);

    int pos=0, cur_class=-1, cur_proto=-1, found=0, epaddr=0, mps=0, interval=1, iface=0;
    while (pos+2 <= wtot) {
        int blen=x_hbuf[pos], btype=x_hbuf[pos+1];
        if (blen<2) break;
        if (btype==4){ iface=x_hbuf[pos+2]; cur_class=x_hbuf[pos+5]; cur_proto=x_hbuf[pos+7]; }
        else if (btype==5 && cur_class==3){
            int ea=x_hbuf[pos+2], attr=x_hbuf[pos+3];
            if ((attr&3)==3 && (ea&0x80)){ epaddr=ea; mps=x_hbuf[pos+4]|(x_hbuf[pos+5]<<8); interval=x_hbuf[pos+6]; found=1; break; }
        }
        pos+=blen;
    }
    if (!found){ uart_puts("[xhci] hid: no interrupt-IN EP\n"); return -2; }
    int dci=(epaddr&0xf)*2+1, proto=cur_proto;
    uart_puts("[xhci] hid: slot="); puts_hex32((unsigned)slot);
    uart_puts(" iface_proto="); puts_hex32((unsigned)proto);
    uart_puts(" epaddr="); puts_hex32((unsigned)epaddr);
    uart_puts(" mps="); puts_hex32((unsigned)mps);
    uart_puts(" dci="); puts_hex32((unsigned)dci); uart_puts("\n");

    if (xs_control_nodata(slot, 0x00, 9, 1, 0)!=0) uart_puts("[xhci] hid: SET_CONFIG failed\n");

    /* interrupt-IN transfer ring */
    struct xhci_trb *r=x_int_ring[slot];
    for (int i=0;i<16;i++){ r[i].p0=r[i].p1=r[i].status=r[i].control=0; }
    r[15].p0=(unsigned)(XDA(r)&0xffffffff); r[15].p1=(unsigned)(XDA(r)>>32);
    r[15].control=(6u<<10)|(1u<<1)|1u;
    x_int_idx[slot]=0; x_int_cyc[slot]=1;

    unsigned char *dctx=x_devctx_s[slot];
    for (unsigned i=0;i<sizeof x_input_ctx;i++) x_input_ctx[i]=0;
    unsigned int *icc=xctx(x_input_ctx,0); icc[1]=(1u<<0)|(1u<<dci);
    unsigned int *sc=xctx(x_input_ctx,1);
    sc[0]=((unsigned)dci<<27)|((unsigned)speed<<20)|(route&0xfffff);
    sc[1]=((unsigned)root_port&0xff)<<16;
    if (tt_hub) sc[2]=((unsigned)tt_hub&0xff)|(((unsigned)tt_port&0xff)<<8);
    unsigned int *ep=xctx(x_input_ctx,dci+1);
    unsigned iv=3; { int b=interval; while (b>1 && iv<10){ b>>=1; iv++; } }
    ep[0]=(iv<<16);
    ep[1]=(7u<<3)|(3u<<1)|((unsigned)mps<<16);                 /* Interrupt-IN, CErr=3, MPS */
    unsigned long trd=XDA(r)|1u;
    ep[2]=(unsigned)(trd&0xffffffff); ep[3]=(unsigned)(trd>>32);
    ep[4]=((unsigned)mps<<16)|((unsigned)mps);
    x_dcbaa[slot]=XDA(dctx); __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(x_input_ctx);
    x_cmd_push((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24));
    struct xhci_trb ev=x_event_wait(33);
    unsigned cc=(ev.status>>24)&0xff;
    uart_puts("[xhci] hid: configure-ep cc="); puts_hex32(cc); uart_puts("\n");
    if (cc!=1) return -3;

    xs_control_nodata(slot, 0x21, 0x0A, 0, iface);             /* SET_IDLE(0) */
    xs_control_nodata(slot, 0x21, 0x0B, 0, iface);             /* SET_PROTOCOL boot */

    x_hid_arm(slot, dci); x_hid_arm(slot, dci);                /* prime two transfers */
    if (proto==1){ x_kbd_slot=slot; x_kbd_dci=dci; x_kbd_iface=iface; uart_puts("[xhci] hid: keyboard armed\n"); }
    else         { x_mouse_slot=slot; x_mouse_dci=dci; x_mouse_iface=iface; uart_puts("[xhci] hid: mouse armed\n"); }
    return 0;
}

/* ===== USB Mass Storage (Bulk-Only Transport) endpoint bring-up =============
 * A USB flash drive / SSD presents interface class 8 (Mass Storage), protocol
 * 0x50 (Bulk-Only Transport), with exactly one Bulk-IN and one Bulk-OUT
 * endpoint.  We configure BOTH in a single Configure-Endpoint command, then the
 * SCSI/BOT layer (device/usb/xhci/usbmsd.c) drives reads/writes through the
 * xhci_msd_bulk_{in,out}() primitives below.  Only one MSD device is tracked
 * (the first one enumerated) — plenty for a single thumb drive on /sd. */
static struct xhci_trb x_bo_ring[XMAXSLOT][16] __attribute__((aligned(64)));   /* Bulk-OUT */
static struct xhci_trb x_bi_ring[XMAXSLOT][16] __attribute__((aligned(64)));   /* Bulk-IN  */
static int x_bo_idx[XMAXSLOT], x_bo_cyc[XMAXSLOT];
static int x_bi_idx[XMAXSLOT], x_bi_cyc[XMAXSLOT];
static int x_msd_slot = -1, x_msd_in_dci, x_msd_out_dci;

int xhci_msd_present(void) { return x_msd_slot >= 0; }

/* Push one Normal TRB on a bulk ring, ring the endpoint doorbell, and wait for
 * its Transfer Event.  `buf` must be in identity-mapped RAM (static aligned
 * buffers — same region the HID DMA buffers live in).  A single TRB can carry a
 * multi-block transfer; the controller packetises it to the endpoint MPS.
 * Returns bytes transferred (len - residue) on success or short packet, else -1. */
static int x_bulk_run(int slot, int dci, struct xhci_trb *ring, int *idx, int *cyc,
                      void *buf, int len)
{
    struct xhci_trb *t = &ring[*idx];
    unsigned long ba = XDA(buf);
    t->p0 = (unsigned)(ba & 0xffffffff);
    t->p1 = (unsigned)(ba >> 32);
    t->status  = (unsigned)len;                               /* TRB transfer length */
    t->control = (1u<<10)|(1u<<5)|((unsigned)(*cyc)&1u);      /* Normal + IOC + cycle */
    __asm__ volatile("dsb sy":::"memory");
    (*idx)++;
    if (*idx == 15) {
        ring[15].control = (ring[15].control & ~1u) | ((unsigned)(*cyc)&1u);
        __asm__ volatile("dsb sy":::"memory");
        *idx = 0; *cyc ^= 1;
    }
    XR(x_db, slot*4) = (unsigned)dci;
    __asm__ volatile("dsb sy":::"memory");
    struct xhci_trb ev = x_event_wait(32);
    unsigned cc = (ev.status>>24)&0xff;
    x_last_xfer_cc = cc; x_xfer_events++;
    if (cc != 1 && cc != 13) return -1;                       /* 1=success 13=short */
    return len - (int)(ev.status & 0xffffff);
}

int xhci_msd_bulk_out(void *buf, int len)
{
    if (x_msd_slot < 0) return -1;
    return x_bulk_run(x_msd_slot, x_msd_out_dci, x_bo_ring[x_msd_slot],
                      &x_bo_idx[x_msd_slot], &x_bo_cyc[x_msd_slot], buf, len);
}
int xhci_msd_bulk_in(void *buf, int len)
{
    if (x_msd_slot < 0) return -1;
    return x_bulk_run(x_msd_slot, x_msd_in_dci, x_bi_ring[x_msd_slot],
                      &x_bi_idx[x_msd_slot], &x_bi_cyc[x_msd_slot], buf, len);
}

/* Clear a Bulk endpoint STALL (BOT phase error recovery): standard CLEAR_FEATURE
 * (ENDPOINT_HALT=0) on EP0 to the endpoint address, then a Reset-Endpoint +
 * Set-TR-Dequeue would be needed for a full xHCI recovery.  For bring-up we do
 * the device-side clear, which suffices for the common "CSW stall" sticks. */
int xhci_msd_clear_halt(int in)
{
    if (x_msd_slot < 0) return -1;
    int dci  = in ? x_msd_in_dci : x_msd_out_dci;
    int epad = in ? (((dci-1)/2) | 0x80) : (dci/2);
    return xs_control_nodata(x_msd_slot, 0x02, 1, 0, (unsigned)epad);   /* CLEAR_FEATURE EP_HALT */
}

/* Configure the Bulk-IN/OUT endpoints of a mass-storage device on `slot` and
 * register it as the active MSD.  Mirrors xhci_hid_setup but adds two bulk EPs.
 * Returns 0 ok, -1 no config desc, -2 not a BOT MSD / no bulk pair, -3 cfg-ep. */
int xhci_msd_setup(int slot, int root_port, unsigned route, int speed, int tt_hub, int tt_port)
{
    int got = xs_control_in(slot, 0x80, 6, (2u<<8), 0, 9);
    if (got < 4 || x_hbuf[1] != 2) { uart_puts("[xhci] msd: no config desc\n"); return -1; }
    int wtot = x_hbuf[2] | (x_hbuf[3]<<8);
    if (wtot > (int)sizeof x_hbuf) wtot = sizeof x_hbuf;
    xs_control_in(slot, 0x80, 6, (2u<<8), 0, wtot);

    int pos=0, in_iface=0, iface=0, cfgval=1;
    int in_ep=0, out_ep=0, in_mps=64, out_mps=64;
    cfgval = (wtot >= 6) ? x_hbuf[5] : 1;                      /* bConfigurationValue */
    while (pos + 2 <= wtot) {
        int blen=x_hbuf[pos], btype=x_hbuf[pos+1];
        if (blen < 2) break;
        if (btype == 4) {                                     /* interface descriptor */
            int icls=x_hbuf[pos+5], iproto=x_hbuf[pos+7];
            in_iface = (icls == 8 && iproto == 0x50);         /* Mass Storage, Bulk-Only */
            if (in_iface) iface = x_hbuf[pos+2];
        } else if (btype == 5 && in_iface) {                  /* endpoint descriptor */
            int ea=x_hbuf[pos+2], attr=x_hbuf[pos+3];
            int mps=x_hbuf[pos+4]|(x_hbuf[pos+5]<<8);
            if ((attr & 3) == 2) {                            /* Bulk */
                if (ea & 0x80) { in_ep=ea; in_mps=mps; }
                else           { out_ep=ea; out_mps=mps; }
            }
        }
        pos += blen;
    }
    if (!in_ep || !out_ep) { uart_puts("[xhci] msd: no bulk EP pair (not a BOT device)\n"); return -2; }
    int in_dci=(in_ep&0xf)*2+1, out_dci=(out_ep&0xf)*2;
    int max_dci = in_dci > out_dci ? in_dci : out_dci;
    uart_puts("[xhci] msd: slot="); puts_hex32((unsigned)slot);
    uart_puts(" iface="); puts_hex32((unsigned)iface);
    uart_puts(" in_ep="); puts_hex32((unsigned)in_ep);
    uart_puts(" out_ep="); puts_hex32((unsigned)out_ep);
    uart_puts(" in_mps="); puts_hex32((unsigned)in_mps);
    uart_puts(" out_mps="); puts_hex32((unsigned)out_mps); uart_puts("\n");

    if (xs_control_nodata(slot, 0x00, 9, (unsigned)cfgval, 0) != 0)
        uart_puts("[xhci] msd: SET_CONFIG failed (continuing)\n");

    /* Build both bulk transfer rings (15 usable TRBs + Link). */
    struct xhci_trb *ro = x_bo_ring[slot];
    struct xhci_trb *ri = x_bi_ring[slot];
    for (int i=0;i<16;i++){ ro[i].p0=ro[i].p1=ro[i].status=ro[i].control=0;
                            ri[i].p0=ri[i].p1=ri[i].status=ri[i].control=0; }
    ro[15].p0=(unsigned)(XDA(ro)&0xffffffff); ro[15].p1=(unsigned)(XDA(ro)>>32); ro[15].control=(6u<<10)|(1u<<1)|1u;
    ri[15].p0=(unsigned)(XDA(ri)&0xffffffff); ri[15].p1=(unsigned)(XDA(ri)>>32); ri[15].control=(6u<<10)|(1u<<1)|1u;
    x_bo_idx[slot]=0; x_bo_cyc[slot]=1;
    x_bi_idx[slot]=0; x_bi_cyc[slot]=1;

    unsigned char *dctx=x_devctx_s[slot];
    for (unsigned i=0;i<sizeof x_input_ctx;i++) x_input_ctx[i]=0;
    unsigned int *icc=xctx(x_input_ctx,0);
    icc[1]=(1u<<0)|(1u<<in_dci)|(1u<<out_dci);                 /* add slot + both EPs */
    unsigned int *sc=xctx(x_input_ctx,1);
    sc[0]=((unsigned)max_dci<<27)|((unsigned)speed<<20)|(route&0xfffff);
    sc[1]=((unsigned)root_port&0xff)<<16;
    if (tt_hub) sc[2]=((unsigned)tt_hub&0xff)|(((unsigned)tt_port&0xff)<<8);

    unsigned int *epi=xctx(x_input_ctx,in_dci+1);
    epi[1]=(6u<<3)|(3u<<1)|((unsigned)in_mps<<16);            /* Bulk-IN, CErr=3, MPS */
    unsigned long tri=XDA(ri)|1u;
    epi[2]=(unsigned)(tri&0xffffffff); epi[3]=(unsigned)(tri>>32);
    epi[4]=(unsigned)in_mps;                                  /* avg TRB len */

    unsigned int *epo=xctx(x_input_ctx,out_dci+1);
    epo[1]=(2u<<3)|(3u<<1)|((unsigned)out_mps<<16);          /* Bulk-OUT, CErr=3, MPS */
    unsigned long tro=XDA(ro)|1u;
    epo[2]=(unsigned)(tro&0xffffffff); epo[3]=(unsigned)(tro>>32);
    epo[4]=(unsigned)out_mps;

    x_dcbaa[slot]=XDA(dctx); __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(x_input_ctx);
    x_cmd_push((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24));
    struct xhci_trb ev=x_event_wait(33);
    unsigned cc=(ev.status>>24)&0xff;
    uart_puts("[xhci] msd: configure-ep cc="); puts_hex32(cc); uart_puts("\n");
    if (cc!=1) return -3;

    x_msd_slot=slot; x_msd_in_dci=in_dci; x_msd_out_dci=out_dci;
    uart_puts("[xhci] msd: bulk endpoints ready\n");
    return 0;
}

/* HID GET_REPORT(Input) on EP0 — for LS/FS devices behind a hub whose periodic
 * (interrupt) transfers don't deliver via the TT, this control-pipe poll
 * reliably reads the current report (control transfers are proven working).
 * Returns bytes read into x_hbuf, or <=0. */
static int xs_get_report(int slot, int iface, int len)
{
    return xs_control_in(slot, 0xA1, 0x01, 0x0100, (unsigned)iface, len);
}

static const char x_km[64] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',[0x0a]='g',[0x0b]='h',
    [0x0c]='i',[0x0d]='j',[0x0e]='k',[0x0f]='l',[0x10]='m',[0x11]='n',[0x12]='o',[0x13]='p',
    [0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',[0x18]='u',[0x19]='v',[0x1a]='w',[0x1b]='x',
    [0x1c]='y',[0x1d]='z',[0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',
    [0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',[0x28]='\n',[0x2a]='\b',[0x2b]='\t',[0x2c]=' ',
    [0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',[0x33]=';',[0x34]='\'',[0x35]='`',
    [0x36]=',',[0x37]='.',[0x38]='/',
};

/* Apply Shift to a base (unshifted) US-layout character: upper-case letters and
 * map the number row / punctuation to their shifted symbols.  Without this,
 * Shift only upper-cased a-z and e.g. the double-quote (Shift+') needed for
 * RUN "name" was unreachable. */
static char x_shift_char(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    switch (c) {
        case '1': return '!';  case '2': return '@';  case '3': return '#';
        case '4': return '$';  case '5': return '%';  case '6': return '^';
        case '7': return '&';  case '8': return '*';  case '9': return '(';
        case '0': return ')';  case '-': return '_';  case '=': return '+';
        case '[': return '{';  case ']': return '}';  case '\\': return '|';
        case ';': return ':';  case '\'': return '"'; case '`': return '~';
        case ',': return '<';  case '.': return '>';  case '/': return '?';
        default:  return c;
    }
}

/* Translate one fresh HID key usage to character(s) and deliver them.
 *   - Ctrl+letter   -> control code (Ctrl+C = 0x03, Ctrl+H = 0x08, ...)
 *   - arrows / nav  -> ANSI escape sequence (ESC [ X) the editors expect
 *   - otherwise     -> x_km, with Shift applied via x_shift_char.
 * Backspace (0x2a) maps through x_km to 0x08. */
static void x_deliver_key(unsigned u, int shift, int ctrl)
{
    extern void xhci_keyboard_event(char);
    /* Navigation / cursor keys (HID 0x4a..0x52) have no x_km ASCII; emit the
     * 3-byte ESC sequence atomically so the editors' esc parser sees it. */
    char nav = 0;
    switch (u) {
        case 0x4a: nav = 'H'; break;   /* Home      */
        case 0x4d: nav = 'F'; break;   /* End       */
        case 0x4b: nav = '5'; break;   /* Page Up   */
        case 0x4e: nav = '6'; break;   /* Page Down */
        case 0x4f: nav = 'C'; break;   /* Right     */
        case 0x50: nav = 'D'; break;   /* Left      */
        case 0x51: nav = 'B'; break;   /* Down      */
        case 0x52: nav = 'A'; break;   /* Up        */
        case 0x4c: xhci_keyboard_event(0x7f); return;   /* Delete -> DEL */
        default: break;
    }
    if (nav) { xhci_keyboard_event(0x1b); xhci_keyboard_event('['); xhci_keyboard_event(nav); return; }

    if (u >= 0x40) return;
    char c = x_km[u];
    if (!c) return;
    if (ctrl && c >= 'a' && c <= 'z') { xhci_keyboard_event((char)(c & 0x1f)); return; }
    if (shift) c = x_shift_char(c);
    xhci_keyboard_event(c);
}

/* Arm keyboard auto-repeat for a freshly-pressed key (the most recent press
 * wins, matching desktop behaviour). */
static void x_kbd_rep_arm(unsigned u, int shift, int ctrl)
{
    extern unsigned long timer_ticks(void);
    x_kbd_rk = u; x_kbd_rk_shift = shift; x_kbd_rk_ctrl = ctrl;
    x_kbd_rk_next = timer_ticks() + KBD_RPT_DELAY;
}

/* Cancel auto-repeat once the repeating key is no longer held in report b[]. */
static void x_kbd_rep_check(const unsigned char *b)
{
    if (!x_kbd_rk) return;
    for (int i=2;i<8;i++) if (b[i]==x_kbd_rk) return;
    x_kbd_rk = 0;
}

/* Called periodically (wm tick): drain the event ring, deliver HID reports. */
static unsigned long x_pump_calls;
void xhci_mouse_pump(void)
{
    extern void xhci_mouse_event(unsigned, int, int);
    extern void xhci_keyboard_event(char);
    x_pump_calls++;
    if (!x_running) return;
    if (x_mouse_slot<0 && x_kbd_slot<0) return;

    /* Keyboard auto-repeat: emit the held key again once its delay/interval has
     * elapsed.  Runs every pump call (not just on a HID event) since a held key
     * produces no further reports. */
    if (x_kbd_rk) {
        extern unsigned long timer_ticks(void);
        unsigned long now = timer_ticks();
        if ((long)(now - x_kbd_rk_next) >= 0) {
            x_deliver_key(x_kbd_rk, x_kbd_rk_shift, x_kbd_rk_ctrl);
            x_kbd_rk_next = now + KBD_RPT_RATE;
        }
    }

    if (x_poll_mode) {
        /* Control-pipe poll: the interrupt EP is silent (LS device, hub TT), but
         * EP0 GET_REPORT works.  Read the mouse, then the keyboard. */
        if (x_mouse_slot>=0) {
            int n = xs_get_report(x_mouse_slot, x_mouse_iface, 4);
            if (n >= 3) {
                unsigned char *b=x_hbuf;
                unsigned btn=b[0]; int dx=(int)(signed char)b[1]; int dy=(int)(signed char)b[2];
                for (int i=0;i<4 && i<8;i++) x_hid_buf[x_mouse_slot][i]=b[i];
                x_mouse_reports++;
                int chg=((int)btn!=(int)x_mouse_prev_btn);
                { extern int basic_is_running(void); extern int basic_has_buttons(void);
                  if ((btn&7) && !(x_mouse_prev_btn&7) && basic_is_running() && !basic_has_buttons()) x_ctrl_c_pending=1; }
                x_mouse_prev_btn = btn;
                /* Deliver on motion, any button held, OR a button-state CHANGE —
                 * so a button RELEASE (btn 0, no motion) reaches xhci_mouse_event
                 * and main.c's right/left edge detectors reset (else the context
                 * menu only opens on the first right-click). */
                if (dx||dy||btn||chg) xhci_mouse_event(btn,dx,dy);
            }
        }
        if (x_kbd_slot>=0) {
            int n = xs_get_report(x_kbd_slot, x_kbd_iface, 8);
            if (n >= 3) {
                unsigned char *b=x_hbuf;
                int shift=(b[0]&0x22)!=0;
                int ctrl=(b[0]&0x11)!=0;
                if (ctrl) for (int i=2;i<8;i++) if (b[i]==0x06) { x_ctrl_c_pending=1; break; }
                for (int i=2;i<8;i++){ unsigned u=b[i]; if (!u) continue;
                    int held=0; for (int j=2;j<8;j++) if (x_kbd_prev[j]==u){held=1;break;}
                    if (held) continue; x_deliver_key(u, shift, ctrl); x_kbd_rep_arm(u, shift, ctrl); }
                x_kbd_rep_check(b);
                for (int i=0;i<8;i++) x_kbd_prev[i]=b[i];
                x_kbd_reports++;
            }
        }
        return;
    }

    for (int guard=0; guard<32; guard++) {
        struct xhci_trb *e=&x_evt[x_evt_idx];
        __asm__ volatile("dsb sy":::"memory");
        if ((e->control & 1u) != (unsigned)x_evt_cycle) break;
        struct xhci_trb ev=*e;
        x_evt_idx++;
        if (x_evt_idx==XEVT_N){ x_evt_idx=0; x_evt_cycle^=1; }
        unsigned long erdp=XDA(&x_evt[x_evt_idx]);
        XR(x_rt,XIR0_ERDP)  =(unsigned)((erdp&0xffffffff)|(1u<<3));
        XR(x_rt,XIR0_ERDP+4)=(unsigned)(erdp>>32);
        if (((ev.control>>10)&0x3f)!=32) continue;             /* Transfer Event */
        int eslot=(int)((ev.control>>24)&0xff);
        int edci =(int)((ev.control>>16)&0x1f);
        x_xfer_events++; x_last_xfer_cc=(ev.status>>24)&0xff;   /* diag: any transfer event */
        x_last_xfer_slot=(unsigned)eslot; x_last_xfer_dci=(unsigned)edci;
        if (eslot==x_mouse_slot && edci==x_mouse_dci){
            unsigned char *b=x_hid_buf[x_mouse_slot];
            unsigned btn=b[0]; int dx=(int)(signed char)b[1]; int dy=(int)(signed char)b[2];
            x_mouse_reports++;
            int chg=((int)btn!=(int)x_mouse_prev_btn);
            /* No working keyboard on this Pi4 (kbd_reports stays 0), so a mouse
             * button is the only way to break an infinite program (e.g. koch).
             * Break on a button-press EDGE while a program runs — never on the
             * level — so the click that LAUNCHED it (still held as the program
             * starts) doesn't immediately break it (e.g. qsort's first WAIT). */
            { extern int basic_is_running(void); extern int basic_has_buttons(void);
              if ((btn&7) && !(x_mouse_prev_btn&7) && basic_is_running() && !basic_has_buttons()) x_ctrl_c_pending=1; }
            x_mouse_prev_btn = btn;
            /* Deliver on motion, any button held, OR a button-state CHANGE so a
             * release (btn 0, no motion) reaches main.c and the right/left click
             * edge detectors reset (else the menu only opens once). */
            if (dx||dy||btn||chg) xhci_mouse_event(btn,dx,dy);
            x_hid_arm(x_mouse_slot, x_mouse_dci);
        } else if (eslot==x_kbd_slot && edci==x_kbd_dci){
            unsigned char *b=x_hid_buf[x_kbd_slot];
            int shift=(b[0]&0x22)!=0;
            int ctrl=(b[0]&0x11)!=0;
            if (ctrl) for (int i=2;i<8;i++) if (b[i]==0x06) { x_ctrl_c_pending=1; break; }
            for (int i=2;i<8;i++){ unsigned u=b[i]; if (!u) continue;
                int held=0; for (int j=2;j<8;j++) if (x_kbd_prev[j]==u){held=1;break;}
                if (held) continue; x_deliver_key(u, shift, ctrl); x_kbd_rep_arm(u, shift, ctrl); }
            x_kbd_rep_check(b);
            for (int i=0;i<8;i++) x_kbd_prev[i]=b[i];
            x_kbd_reports++;
            x_hid_arm(x_kbd_slot, x_kbd_dci);
        }
    }
}
unsigned long xhci_mouse_reports(void){ return x_mouse_reports; }
unsigned long xhci_kbd_reports(void)  { return x_kbd_reports; }
int           xhci_mouse_ok(void)     { return x_mouse_slot>=0; }

/* Did the user press Ctrl+C?  A long-running BASIC program blocks the wm pump
 * (single thread), so the RUN loop calls this to honour Ctrl-C break.  The
 * keyboard runs in interrupt mode (EP0 GET_REPORT is unreliable mid-run), so
 * we drain the HID event ring here — x_ctrl_c_pending is set by the report
 * processing when it sees Ctrl + 'c' — then return and clear that sticky flag.
 * Keyboard reports drained are NOT dispatched (xhci_keyboard_event drops them
 * while a program runs), so this can't re-enter the interpreter. */
int xhci_poll_ctrl_c(void)
{
    if (x_kbd_slot < 0 && x_mouse_slot < 0) return x_ctrl_c_pending;
    xhci_mouse_pump();                 /* drain fresh HID reports -> sets flag */
    int v = x_ctrl_c_pending;
    x_ctrl_c_pending = 0;
    return v;
}
unsigned long xhci_pump_calls(void)   { return x_pump_calls; }
unsigned int  xhci_mfindex(void)      { return x_running ? (XR(x_rt,0)&0x3fff) : 0; }
int           xhci_mouse_slot_dbg(void){ return x_mouse_slot; }
unsigned int  xhci_mouse_bufbyte(int i){ return (x_mouse_slot>=0 && i>=0 && i<8) ? x_hid_buf[x_mouse_slot][i] : 0; }
/* EP1/interrupt endpoint state of the mouse slot (0=disabled,1=running,2=halted,3=stopped,4=error) */
unsigned int  xhci_mouse_ep_state(void){ return (x_mouse_slot>=0) ? (xctx(x_devctx_s[x_mouse_slot], x_mouse_dci)[0] & 0x7) : 0; }
unsigned int  xhci_xfer_events(void)  { return x_xfer_events; }
unsigned int  xhci_last_xfer_cc(void) { return x_last_xfer_cc; }
unsigned int  xhci_last_xfer_sd(void) { return (x_last_xfer_slot<<8)|x_last_xfer_dci; }

/* Firmware-proxied probe of the addresses our start4.elf disassembly
 * identified as PCIe-init gating points.  Each row reports the value AND
 * the mailbox response code: 0x80000004 = firmware handled (value valid),
 * 0 = firmware ignored (value is stale buffer).  An ignored response on
 * a peripheral that we KNOW exists means the firmware's address allow-list
 * doesn't include it — strong signal about what is/isn't routed.
 *
 * The headline target is 0x7E0000B4 bit 0 — disasm shows firmware's PCIe
 * init code at ec63ff2-ec63ff6 reads (0x7E000080 + 0x34) and gives up
 * unless bit 0 is set.  If we can SEE this bit via mailbox, we know our
 * gp+5476 deduction is sound.  If we can WRITE this bit, we have a way to
 * push firmware down the PCIe-init path without touching the gp register
 * directly. */
/* Single-address probe — caller passes the address; we mailbox-read and
 * format.  NO UART tracing inside (uart_putc fans out to screen_putc and
 * shellwin_record_char which appeared to deadlock in HTTP-handler context).
 * Mirror /cprman's exact pattern instead — a known-good caller. */
int xhci_pcie_fw_probe_one(char *out, int max, unsigned int addr)
{
    int p = 0;
    p = s_put(out, p, max, "probe addr=");
    p = s_puthex32(out, p, max, addr);
    p = s_put(out, p, max, " ");

    unsigned int v = 0, r = 0;
    int rc = xhci_periph_read(addr, &v, &r);

    if (rc != 0) {
        p = s_put(out, p, max, "MBOX-FAIL");
    } else {
        p = s_put(out, p, max, "val=");
        p = s_puthex32(out, p, max, v);
        p = s_put(out, p, max, " resp=");
        p = s_puthex32(out, p, max, r);
        if (r == 0)                    p = s_put(out, p, max, " [IGNORED]");
        else if (r == 0x80000008U)     p = s_put(out, p, max, " [HANDLED]");
        else if (r & 0x80000000U)      p = s_put(out, p, max, " [REPLIED]");
    }
    p = s_put(out, p, max, "\n");
    if (p < max) out[p] = 0;
    return p;
}

/* Full sweep — kept for callers that already trust the mailbox.  Reads
 * the same address list as the per-address probe to make selective
 * recall easy in shell scripts. */
int xhci_pcie_fw_probe(char *out, int max)
{
    /* PROVEN-SAFE-ONLY sweep — these are the exact addresses that the
     * /cprman route successfully reads in production.  Any address NOT
     * in this list must be tried one-at-a-time via /pcie-fw-probe1?addr=...
     * because firmware can wedge the AXI bus on unmapped addresses (and
     * the wedge stalls ARM instruction fetch too, defeating mbox_call's
     * own timeout). */
    int p = 0;
    static const unsigned int addrs[] = {
        0x7E101128U,  /* CPRMAN PCIe CTL — confirmed good via /cprman */
        0xFE101128U,  /* same, ARM view — confirmed good via /cprman */
        0x7E1011D0U,  /* EMMC2 CTL — confirmed good via /cprman */
    };
    for (unsigned i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
        p += xhci_pcie_fw_probe_one(out + p, max - p, addrs[i]);
    }
    if (p < max) out[p] = 0;
    return p;
}

/* Force the firmware's PCIe-present gate at 0x7E0000B4 bit 0.  If SET_PERIPH_REG
 * actually writes to peripherals (vs. only allow-listed addresses), the next
 * call from firmware's own init path will see bit 0 set and continue.  We
 * cannot trigger firmware re-entry from here — but if a subsequent xHCI reset
 * mailbox call (or anything that re-runs the gate check) succeeds, the link
 * may finally come up.  Reads back BOTH the gate AND CPRMAN PCIe CTL so we
 * can see whether anything followed on. */
int xhci_pcie_fw_gate_force(char *out, int max)
{
    int p = 0;
    unsigned int v = 0, r = 0;

    /* Read current gate value */
    xhci_periph_read(0x7E0000B4, &v, &r);
    p = s_put(out, p, max, "gate before: val=");
    p = s_puthex32(out, p, max, v); p = s_put(out, p, max, " resp=");
    p = s_puthex32(out, p, max, r); p = s_put(out, p, max, "\n");

    /* Write back with bit 0 set, preserving everything else */
    unsigned int newv = v | 1U;
    int wrc = xhci_periph_write(0x7E0000B4, newv);
    p = s_put(out, p, max, "write 0x7E0000B4 |= 1 rc=");
    p = s_puthex32(out, p, max, (unsigned int)wrc); p = s_put(out, p, max, "\n");

    /* Read back */
    xhci_periph_read(0x7E0000B4, &v, &r);
    p = s_put(out, p, max, "gate after : val=");
    p = s_puthex32(out, p, max, v); p = s_put(out, p, max, " resp=");
    p = s_puthex32(out, p, max, r); p = s_put(out, p, max, "\n");

    /* Re-poll CPRMAN PCIe CTL to see if anything propagated */
    xhci_periph_read(0x7E101128, &v, &r);
    p = s_put(out, p, max, "CPRMAN PCIe CTL after: val=");
    p = s_puthex32(out, p, max, v); p = s_put(out, p, max, " resp=");
    p = s_puthex32(out, p, max, r); p = s_put(out, p, max, "\n");

    if (p < max) out[p] = 0;
    return p;
}

int xhci_pcie_dump_html(char *out, int max)
{
    int p = 0;
    static const struct { const char *name; unsigned int off; } regs[] = {
        { "vendor[0x000] ", PCIE_RC_CFG_VENDOR   },
        { "class [0x008] ", PCIE_RC_CFG_CLASS    },
        { "ctrl  [0x4008]", PCIE_MISC_CTRL       },
        { "stat  [0x4068]", PCIE_MISC_STATUS     },
        { "rev   [0x406C]", PCIE_MISC_REVISION   },
        { "dbg   [0x4204]", PCIE_MISC_HARD_DBG   },
        { "lcap  [0x04DC]", PCIE_RC_CFG_LINK_CAP },
    };
    p = s_put(out, p, max, "pcie_base=");
    p = s_puthex32(out, p, max, (unsigned int)PCIE_BASE);
    p = s_put(out, p, max, "\n");
    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        p = s_put(out, p, max, regs[i].name);
        p = s_put(out, p, max, " = ");
        unsigned int v;
        if (safe_mmio_read32(PCIE_BASE + regs[i].off, &v) == 0) {
            p = s_puthex32(out, p, max, v);
        } else {
            p = s_put(out, p, max, "FAULT");
        }
        p = s_put(out, p, max, "\n");
    }
    if (p < max) out[p] = 0;
    return p;
}

#endif /* PCIE_BASE */

#ifndef PCIE_BASE
int xhci_notify_reset_call(void)             { return -1; }
int xhci_pcie_dump_html(char *out, int max)
{
    const char *s = "pcie: not supported on this build\n";
    int p = 0; while (*s && p < max - 1) out[p++] = *s++;
    if (p < max) out[p] = 0;
    return p;
}
/* Stubs for everything tcp_server.c calls via `extern` — without these
 * the QEMU build (which has no PCIE_BASE) won't link. */
int xhci_pcie_bring_up(void)                       { return -1; }
int xhci_pcie_enum_vl805(void)                     { return -1; }
int xhci_vl805_init(void)                          { return -1; }
int xhci_vl805_enum_mouse(void)                    { return -1; }
int xhci_vl805_enum_full(void)                     { return -1; }
int xhci_hub_enumerate(int a,int b,int c)          { (void)a;(void)b;(void)c; return -1; }
int xhci_hid_setup(int a,int b,unsigned c,int d,int e,int f){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1; }
void xhci_mouse_pump(void)                          { }
unsigned long xhci_pump_calls(void)                 { return 0; }
unsigned long xhci_mouse_reports(void)              { return 0; }
unsigned long xhci_kbd_reports(void)                { return 0; }
int           xhci_poll_ctrl_c(void)                { return 0; }
unsigned int  xhci_mfindex(void)                    { return 0; }
int           xhci_mouse_slot_dbg(void)             { return -1; }
unsigned int  xhci_mouse_ep_state(void)             { return 0; }
unsigned int  xhci_mouse_bufbyte(int i)             { (void)i; return 0; }
unsigned int  xhci_xfer_events(void)                { return 0; }
unsigned int  xhci_last_xfer_cc(void)               { return 0; }
unsigned int  xhci_last_xfer_sd(void)               { return 0; }
int xhci_periph_read(unsigned int a, unsigned int *o, unsigned int *r)
                                                   { (void)a; if (o) *o = 0; if (r) *r = 0; return -1; }
int xhci_periph_write(unsigned int a, unsigned int v)  { (void)a; (void)v; return -1; }
int xhci_firmware_revision(unsigned int *o, unsigned int *r)
                                                   { if (o) *o = 0; if (r) *r = 0; return -1; }
unsigned int xhci_cprman_read(unsigned int off)    { (void)off; return 0; }
int xhci_cprman_enable_pcie(void)                  { return -1; }
int xhci_cprman_enable_pcie_src(unsigned int src)  { (void)src; return -1; }
int xhci_cprman_pcie_axi_enable(void)              { return -1; }
int xhci_pcie_fw_probe(char *out, int max)
{ const char *s = "fw-probe: not supported\n"; int p=0; while(*s && p<max-1) out[p++]=*s++; if(p<max) out[p]=0; return p; }
int xhci_pcie_fw_gate_force(char *out, int max)
{ const char *s = "fw-gate-force: not supported\n"; int p=0; while(*s && p<max-1) out[p++]=*s++; if(p<max) out[p]=0; return p; }
int xhci_pcie_clk_full_sequence(void)              { return -1; }
int xhci_pcie_fw_probe_one(char *out, int max, unsigned int addr)
{ (void)addr; const char *s="probe: not supported\n"; int p=0; while(*s && p<max-1) out[p++]=*s++; if(p<max) out[p]=0; return p; }
#endif
