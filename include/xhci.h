// include/xhci.h — VL805 (PCIe xHCI) driver for Pi 4 USB-A ports.
//
// Pi 4 routes its four USB-A sockets through VL805, a Via Labs
// xHCI host controller hanging off the BCM2711 PCIe-1 controller.
// USPi (DWC2-only) can't reach those ports, so we roll our own.
//
// Phase plan (one shell `pcie` + `xhci` step per session):
//   XHCI-A  PCIe controller MMIO read (revision register)
//   XHCI-B  ECAM access: read VL805 vendor / device / BAR0
//   XHCI-C  VL805 firmware load via VC mailbox (notify-xhci-reset)
//   XHCI-D  xHCI Capability + Operational registers + reset
//   XHCI-E  DCBAA + Command Ring + Event Ring
//   XHCI-F  Port reset + Address Device
//   XHCI-G  Control transfer (GET_DESCRIPTOR / SET_CONFIGURATION)
//   XHCI-H  HID Interrupt transfer → cursor + shell input

#ifndef XINU_RPI4_XHCI_H
#define XINU_RPI4_XHCI_H

#ifdef PCIE_BASE   /* Pi 4 only — PI4_CFLAGS supplies the value */

/* Top-level bring-up: at XHCI-A this just probes the PCIe
 * controller revision register.  Later phases extend the body. */
void xhci_init(void);

/* PCIe controller diagnostic — read by the shell `pcie` command. */
unsigned int xhci_pcie_revision(void);

#else  /* PCIE_BASE not defined */

static inline void         xhci_init(void)         {}
static inline unsigned int xhci_pcie_revision(void){ return 0; }

#endif

/* On-demand diagnostics, callable from any context (used by /pcie + /xhci-reset
 * HTTP routes).  These tolerate PCIE_BASE undefined (return a stub message / -1)
 * so a single source tree builds for all boards. */
int xhci_pcie_dump_html(char *out, int max);   /* writes text dump, returns length */
int xhci_notify_reset_call(void);              /* VC mailbox notify-xhci-reset; rc */

#endif /* XINU_RPI4_XHCI_H */
