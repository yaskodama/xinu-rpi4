// device/usb/dwc2.c — DWC2 USB 2.0 host controller driver, M0 stage.
//
// Round 1 phase USB-M0: power on the USB HCD via the VC mailbox
// (mandatory on Pi 4 even though the BCM2711 power-on default is
// already "USB HCD on" — explicit is safer), then probe the
// Synopsys ID register at USB_BASE + 0x40 to confirm the
// controller answers MMIO.  Nothing else is touched yet: no core
// reset, no host-port reset, no enumeration.  Those come in
// USB-M1+ once we've verified the controller is alive.
//
// References used:
//   - BCM2711 ARM peripherals datasheet, §4 USB
//   - Synopsys DesignWare HS OTG USB 2.0 controller databook,
//     §4 "Global CSR map" for register offsets
//   - rsta2/uspi (the canonical bare-metal RPi USB stack)
//
// Layout convention: only USB_BASE-dependent code lives inside
// #ifdef USB_BASE.  When the macro isn't defined (Pi 5, QEMU),
// every public function compiles down to a no-op so the link
// step still succeeds and the shell prints a clean message.

#include "usb.h"
#include "uart.h"
#include "mbox.h"

#ifdef USB_BASE

/* ---- DWC2 register file --------------------------------------- */
/* All offsets relative to USB_BASE.  Only the few we need at M0
 * are listed; M1+ will fill in GRSTCTL, GAHBCFG, GINTMSK, … */
#define DWC2_REG(off)    (*(volatile unsigned int *)(USB_BASE + (off)))
#define DWC2_GOTGCTL     DWC2_REG(0x000)
#define DWC2_GAHBCFG     DWC2_REG(0x008)
#define DWC2_GUSBCFG     DWC2_REG(0x00C)
#define DWC2_GRSTCTL     DWC2_REG(0x010)
#define DWC2_GINTSTS     DWC2_REG(0x014)
#define DWC2_GSNPSID     DWC2_REG(0x040)
#define DWC2_GHWCFG1     DWC2_REG(0x044)
#define DWC2_GHWCFG2     DWC2_REG(0x048)
#define DWC2_GHWCFG3     DWC2_REG(0x04C)
#define DWC2_GHWCFG4     DWC2_REG(0x050)

#define DWC2_SYNOPSYS_SIG  0x4F540000u   /* upper 16 bits of GSNPSID */

/* VC mailbox "set power state" tag for the USB HCD.
 *
 * Property tag layout (16-byte aligned):
 *   [0] total buffer size in bytes
 *   [1] request/response code (0 on request, 0x80000000 on success)
 *   [2] tag id  (0x00028001 = set power state)
 *   [3] value buffer size in bytes
 *   [4] request indicator (0) / response indicator
 *   [5] device id  (3 = USB HCD)
 *   [6] state      (bit 0 = on, bit 1 = wait for stable)
 *   [7] end tag (0)
 */
static volatile unsigned int __attribute__((aligned(16))) pwr_buf[8];

/* Latched outcome of the last usb_init() so the shell can render
 * "powered + signature ok" vs "no MMIO" without re-poking. */
static int g_init_ok;

static int usb_power_on(void)
{
    pwr_buf[0] = 8 * sizeof(unsigned int);
    pwr_buf[1] = 0;                /* request code */
    pwr_buf[2] = 0x00028001;       /* set power state */
    pwr_buf[3] = 8;                /* value buffer size */
    pwr_buf[4] = 0;                /* request indicator */
    pwr_buf[5] = 3;                /* device id = USB HCD */
    pwr_buf[6] = 3;                /* state = on | wait */
    pwr_buf[7] = 0;                /* end tag */
    return mbox_call(pwr_buf);
}

void usb_init(void)
{
    int rc;
    unsigned int id;

    g_init_ok = 0;

    rc = usb_power_on();
    if (rc != 0) {
        uart_puts("usb: VC mailbox power-on failed; MMIO probe skipped\n");
        /* Don't read DWC2 MMIO without confirmed power — without
         * exception vectors a data abort here would brick the
         * kernel before wm_run() ever paints the desktop. */
        return;
    }
    uart_puts("usb: VC mailbox reports HCD powered on\n");

    id = DWC2_GSNPSID;
    if ((id & 0xFFFF0000u) == DWC2_SYNOPSYS_SIG) {
        uart_puts("usb: DWC2 alive, GSNPSID=0x");
        /* small inline hex printer to avoid pulling shell helpers */
        for (int i = 7; i >= 0; i--) {
            unsigned int nyb = (id >> (i * 4)) & 0xF;
            uart_putc((char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10)));
        }
        uart_puts("\n");
        g_init_ok = 1;
    } else {
        uart_puts("usb: DWC2 GSNPSID mismatch (read 0x");
        for (int i = 7; i >= 0; i--) {
            unsigned int nyb = (id >> (i * 4)) & 0xF;
            uart_putc((char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10)));
        }
        uart_puts("), controller absent or unpowered\n");
    }
}

int usb_present(void) { return 1; }
unsigned int usb_synopsys_id(void) { return DWC2_GSNPSID; }
int usb_last_init_ok(void) { return g_init_ok; }

#else  /* USB_BASE not defined — Pi 5 / QEMU virt */

void usb_init(void) { /* no DWC2 on this board */ }
int usb_present(void) { return 0; }
unsigned int usb_synopsys_id(void) { return 0; }
int usb_last_init_ok(void) { return 0; }

#endif
