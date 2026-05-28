// include/early_diag.h — visible "the kernel is alive" diagnostic.
//
// Pi 5 boot has a frustrating failure mode where the firmware shows
// a full-screen rainbow test pattern, our kernel image is loaded by
// the bootloader, but we see no evidence the kernel actually starts
// running (UART silent, mailbox-allocated FB invisible).
//
// early_paint_diagnostic() is called as the very first thing in
// kernel_main.  It writes large, distinct colour blocks to several
// candidate physical addresses that *could* be the firmware's
// framebuffer base, and cleans the D-cache so the writes reach
// memory even if the firmware enabled the MMU.  Whichever address
// happens to overlap the real FB will visibly stamp its colour
// over the rainbow — telling us both "kernel runs" and "FB lives
// somewhere around HERE".

#ifndef XINU_RPI4_EARLY_DIAG_H
#define XINU_RPI4_EARLY_DIAG_H

/* Phys addr the firmware passed in x0 (saved by boot.S after BSS
 * clear).  Reading it before main() is safe because boot.S writes
 * to a .data location that survives the BSS-zero loop. */
extern unsigned long dtb_addr;

void early_paint_diagnostic(void);

#endif /* XINU_RPI4_EARLY_DIAG_H */
