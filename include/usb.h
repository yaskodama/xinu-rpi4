// include/usb.h — DWC2 USB HCD driver interface (Pi 4 only).
//
// Pi 4 BCM2711 exposes a Synopsys DesignWare DWC2 USB 2.0 OTG
// controller at peripheral_base + 0x980000 = 0xFE980000.  This is
// the USB1 host port physically wired to the USB-C power connector
// (the four USB-A sockets go via VL805 xHCI on PCIe, which we
// don't touch).  To use a USB-A mouse it must be plugged into an
// OTG cable (USB-C ↔ USB-A).
//
// Pi 5 routes USB through RP1 (PCIe), and QEMU virt has no DWC2
// at our MMIO address.  Both make every function a no-op so a
// single source tree can build for all three boards.

#ifndef XINU_RPI5_USB_H
#define XINU_RPI5_USB_H

/* Power on the USB HCD via VC mailbox (Pi 4 only) and read the
 * Synopsys ID register so subsequent shell diagnostics know
 * whether the controller responded.  Safe to call when USB_BASE
 * isn't defined — it just returns. */
void usb_init(void);

/* True if this build has a DWC2 USB HCD mapped at USB_BASE.  Used
 * by the shell to decide whether to print a useful error or
 * actually query MMIO. */
int usb_present(void);

/* Read DWC2 GSNPSID (offset 0x40).  Returns 0 when usb_present()
 * is false.  Successful read should yield 0x4F54xxxx (Synopsys
 * signature in the upper 16 bits, core release in the lower 16). */
unsigned int usb_synopsys_id(void);

/* Whether the last usb_init() saw a valid GSNPSID signature.
 * Latched into a static so the shell can show "powered" vs
 * "no-response" without re-reading MMIO. */
int usb_last_init_ok(void);

#endif /* XINU_RPI5_USB_H */
