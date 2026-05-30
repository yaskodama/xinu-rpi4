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
    /* Property tag 0x00030058 (notify-xhci-reset).  Previous attempt with
     * devid=0x00100000 hung the firmware (mailbox call never returned -> all
     * subsequent mailbox calls also dead).  Try devid=0 (firmware default,
     * which the original comment said was the fallback): on Pi 4 firmware,
     * VL805 is the only xHCI client of this tag, so default-targets-VL805
     * should work — if it does we'll know the encoding was the issue, and
     * if it hangs again the tag itself is the wrong path on this firmware
     * and we need self-bring-up (CPRMAN + brcmstb-pcie). */
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;
    buf[1] = 0;
    buf[2] = 0x00030058U;        /* notify-xhci-reset */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;                  /* devid: 0 = firmware default (VL805) */
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
#define PCIE_EXT_CFG_DATA                0x9004

#define RGR1_SW_INIT_1_INIT_GENERIC_MASK 0x2      /* bit 1: bridge reset */
#define RGR1_SW_INIT_1_PERST_MASK        0x1      /* bit 0: PERST# (generic) */
#define HARD_PCIE_HARD_DEBUG_SERDES_IDDQ 0x08000000  /* bit 27 */
#define MISC_CTRL_SCB_ACCESS_EN          0x00001000
#define MISC_CTRL_CFG_READ_UR_MODE       0x00002000
#define MISC_CTRL_MAX_BURST_SIZE_MASK    0x00300000
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

/* Run the BCM2711 PCIe RC bring-up.  Returns: 0 = link up, -1 = MMIO fault,
 * -2 = link never came up, -3 = controller still all-zero after setup. */
int xhci_pcie_bring_up(void)
{
    unsigned int tmp;

    /* Sanity probe: is the controller responding at all (not faulting)? */
    if (safe_mmio_read32(PCIE_BASE + PCIE_RGR1_SW_INIT_1, &tmp) < 0) return -1;

    /* 1. Reset the bridge */
    if (pcie_bridge_sw_init_set(1) < 0) return -1;
    /* 2. Assert PERST# (some bootloaders may deassert it) */
    if (pcie_perst_set(1) < 0) return -1;
    delay_ms(1);                /* > 100us per Linux */

    /* 3. Take the bridge out of reset */
    if (pcie_bridge_sw_init_set(0) < 0) return -1;

    /* 4. Clear SerDes IDDQ — power up the SerDes block */
    if (safe_mmio_read32(PCIE_BASE + PCIE_MISC_HARD_DEBUG_OFF, &tmp) < 0) return -1;
    tmp &= ~HARD_PCIE_HARD_DEBUG_SERDES_IDDQ;
    if (safe_mmio_write32(PCIE_BASE + PCIE_MISC_HARD_DEBUG_OFF, tmp) < 0) return -1;
    delay_ms(1);                /* SerDes stable */

    /* 5. MISC_CTRL: enable SCB access, CFG read UR mode, set burst=0 (BCM2711) */
    if (safe_mmio_read32(PCIE_BASE + PCIE_MISC_MISC_CTRL_OFF, &tmp) < 0) return -1;
    tmp |= MISC_CTRL_SCB_ACCESS_EN | MISC_CTRL_CFG_READ_UR_MODE;
    tmp &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;   /* burst = 0 = 128 bytes (BCM2711) */
    if (safe_mmio_write32(PCIE_BASE + PCIE_MISC_MISC_CTRL_OFF, tmp) < 0) return -1;

    /* 6. Deassert PERST# — releases VL805 from reset and starts link training */
    if (pcie_perst_set(0) < 0) return -1;

    /* 7. Wait for DL_ACTIVE (link layer up).  Linux waits up to ~100ms.  */
    for (int i = 0; i < 100; i++) {
        if (safe_mmio_read32(PCIE_BASE + PCIE_MISC_PCIE_STATUS_OFF, &tmp) < 0) return -1;
        if (tmp & PCIE_STATUS_DL_ACTIVE) return 0;     /* success */
        delay_ms(1);
    }
    return -2;                  /* timed out waiting for link */
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
#endif
