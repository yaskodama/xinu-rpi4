// include/usbmsd.h — USB Mass Storage (Bulk-Only Transport + SCSI) over xHCI.
//
// Sits on top of the bulk-endpoint primitives in device/usb/xhci/xhci.c
// (xhci_msd_*).  A USB flash drive / SSD enumerated on a VL805 root-port hub
// as interface class 8, protocol 0x50 has its two bulk endpoints configured by
// xhci_msd_setup(); this layer then speaks SCSI (INQUIRY / READ CAPACITY(10) /
// READ(10) / WRITE(10)) wrapped in the Bulk-Only Transport CBW/CSW envelope, and
// exposes a 512-byte block read/write API that fat32_mount_dev() binds for /sd.

#ifndef XINU_RPI4_USBMSD_H
#define XINU_RPI4_USBMSD_H

/* ---- xHCI bulk primitives (implemented in device/usb/xhci/xhci.c) ---- */
int xhci_msd_present(void);                 /* non-zero once a BOT MSD is configured */
int xhci_msd_bulk_out(void *buf, int len);  /* bytes sent, or -1            */
int xhci_msd_bulk_in (void *buf, int len);  /* bytes received, or -1        */
int xhci_msd_clear_halt(int in);            /* CLEAR_FEATURE EP_HALT, in?IN:OUT */

/* ---- SCSI / Bulk-Only Transport layer (device/usb/xhci/usbmsd.c) ---- */

/* Probe the configured MSD: TEST UNIT READY (with retries), INQUIRY, and
 * READ CAPACITY(10).  Logs the vendor/product string and capacity.  Returns 0
 * if the unit is ready and a capacity was read, else -1.  Safe to call when no
 * MSD is present (returns -1). */
int usbmsd_init(void);

/* True once usbmsd_init() has succeeded and the block API is usable. */
int usbmsd_ready(void);

/* Logical block count and block size (512 for every device we support). */
unsigned long usbmsd_block_count(void);
unsigned int  usbmsd_block_size(void);

/* Read / write one 512-byte logical block.  Signatures match fat32_read_fn /
 * fat32_write_fn so they can be handed straight to fat32_mount_dev().  Return 0
 * on success, -1 on error. */
int usbmsd_read_block(unsigned long lba, void *buf);
int usbmsd_write_block(unsigned long lba, const void *buf);

#endif /* XINU_RPI4_USBMSD_H */
