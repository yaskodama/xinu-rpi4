// include/mbox.h — VideoCore mailbox driver interface.
//
// Standard "property tags" channel (channel 8) for talking to the
// firmware about HDMI / framebuffer / clocks / etc.  Works on every
// Pi generation that exposes the legacy VC mailbox; we use it here
// to ask the Pi 4 firmware for a framebuffer it has already set up.
//
// The MMIO base address on the Pi 4 (BCM2711) is peripheral base
// 0xFE000000 + the conventional 0xB880 offset = 0xFE00B880.  Override
// MBOX_BASE via the Makefile (-DMBOX_BASE=0x...UL) if needed.

#ifndef XINU_RPI4_MBOX_H
#define XINU_RPI4_MBOX_H

#ifndef MBOX_BASE
#define MBOX_BASE  0x107C00B880UL
#endif

/* Send a property-tag buffer.  `buf` must be 16-byte aligned and
 * formatted per the VC mailbox property interface.  Returns 0 on
 * success, -1 on timeout (no response from firmware).  Channel 8
 * is implicit. */
int mbox_call(volatile unsigned int *buf);

#endif /* XINU_RPI4_MBOX_H */
