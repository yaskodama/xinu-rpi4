// device/usb/xhci/usbmsd.c — USB Mass Storage (Bulk-Only Transport + SCSI).
//
// The xHCI driver (xhci.c) enumerates a USB flash drive / SSD as interface
// class 8, protocol 0x50, configures its Bulk-IN/OUT endpoints, and exposes
// xhci_msd_bulk_{in,out}().  This file speaks the USB Mass Storage Bulk-Only
// Transport: every command is a 31-byte CBW on the bulk-OUT pipe, an optional
// data phase, then a 13-byte CSW on the bulk-IN pipe.  The command blocks are
// SCSI (INQUIRY / READ CAPACITY(10) / READ(10) / WRITE(10)), giving a 512-byte
// logical-block API that fat32_mount_dev() binds to mount FAT32 under /sd.
//
// One device, LUN 0 — plenty for a single thumb drive.  All DMA buffers are
// 64-byte aligned statics in the identity-mapped low RAM the VL805 reaches over
// PCIe (same region as the HID buffers), so no cache maintenance is needed
// (D-cache is off on this kernel).

#include "usbmsd.h"
#include "uart.h"

extern void delay_ms(unsigned int ms);

/* ---- tiny local hex printer (house style: each file carries its own) ---- */
static void msd_hex(unsigned long v)
{
    char b[16]; int n = 0;
    uart_puts("0x");
    if (v == 0) { uart_putc('0'); return; }
    while (v) { int d = (int)(v & 0xf); b[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; }
    while (n--) uart_putc(b[n]);
}
static void msd_dec(unsigned long v)
{
    char b[24]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(b[n]);
}

/* ---- Bulk-Only Transport envelope ---- */
#define CBW_SIG   0x43425355u   /* "USBC" */
#define CSW_SIG   0x53425355u   /* "USBS" */

static unsigned char msd_cbw[64]  __attribute__((aligned(64)));
static unsigned char msd_csw[64]  __attribute__((aligned(64)));
static unsigned char msd_buf[512] __attribute__((aligned(64)));   /* one logical block */

static unsigned int  msd_tag = 1;
static int           msd_is_ready = 0;
static unsigned long msd_blocks = 0;
static unsigned int  msd_bsize  = 512;

static void put32le(unsigned char *p, unsigned int v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static unsigned int get32le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24);
}
static unsigned int get32be(const unsigned char *p)
{
    return ((unsigned)p[0]<<24) | ((unsigned)p[1]<<16) | ((unsigned)p[2]<<8) | (unsigned)p[3];
}

/* Run one SCSI command through BOT.  `cdb`/`cdblen` is the SCSI command block,
 * `data`/`datalen` the data phase (NULL/0 for none), `dir_in` its direction.
 * Returns the CSW status (0=pass, 1=fail, 2=phase error) or -1 on transport
 * failure. */
static int bot_xfer(const unsigned char *cdb, int cdblen,
                    void *data, int datalen, int dir_in)
{
    unsigned int tag = msd_tag++;

    /* --- CBW (31 bytes) on bulk-OUT --- */
    for (int i = 0; i < 31; i++) msd_cbw[i] = 0;
    put32le(&msd_cbw[0], CBW_SIG);
    put32le(&msd_cbw[4], tag);
    put32le(&msd_cbw[8], (unsigned)datalen);
    msd_cbw[12] = dir_in ? 0x80 : 0x00;        /* bmCBWFlags */
    msd_cbw[13] = 0;                            /* LUN 0      */
    msd_cbw[14] = (unsigned char)(cdblen & 0x1f);
    for (int i = 0; i < cdblen && i < 16; i++) msd_cbw[15+i] = cdb[i];

    if (xhci_msd_bulk_out(msd_cbw, 31) != 31) {
        /* OUT pipe stalled on the CBW — clear and bail (caller may retry). */
        xhci_msd_clear_halt(0);
        return -1;
    }

    /* --- data phase --- */
    if (datalen > 0 && data) {
        int got;
        if (dir_in) {
            got = xhci_msd_bulk_in(data, datalen);
            if (got < 0) xhci_msd_clear_halt(1);   /* clear IN stall, still read CSW */
        } else {
            got = xhci_msd_bulk_out(data, datalen);
            if (got < 0) xhci_msd_clear_halt(0);
        }
    }

    /* --- CSW (13 bytes) on bulk-IN --- */
    for (int i = 0; i < 13; i++) msd_csw[i] = 0;
    int cn = xhci_msd_bulk_in(msd_csw, 13);
    if (cn < 0) {
        /* IN pipe stalled on the CSW: clear the halt and retry once. */
        xhci_msd_clear_halt(1);
        cn = xhci_msd_bulk_in(msd_csw, 13);
        if (cn < 0) return -1;
    }
    if (get32le(&msd_csw[0]) != CSW_SIG)       return -1;   /* bad CSW signature */
    if (get32le(&msd_csw[4]) != tag)           return -1;   /* tag mismatch      */
    return (int)msd_csw[12];                                /* bCSWStatus        */
}

/* ---- SCSI commands ---- */
static int scsi_tur(void)
{
    unsigned char cdb[6] = {0,0,0,0,0,0};      /* TEST UNIT READY */
    return bot_xfer(cdb, 6, 0, 0, 1);
}
static int scsi_request_sense(void)
{
    unsigned char cdb[6] = {0x03,0,0,0,18,0};  /* REQUEST SENSE, 18 bytes */
    return bot_xfer(cdb, 6, msd_buf, 18, 1);
}
static int scsi_inquiry(void)
{
    unsigned char cdb[6] = {0x12,0,0,0,36,0};  /* INQUIRY, 36 bytes */
    int st = bot_xfer(cdb, 6, msd_buf, 36, 1);
    if (st == 0) {
        uart_puts("[usbmsd] INQUIRY: '");
        for (int i = 8; i < 36; i++) {         /* vendor(8..15)+product(16..31)+rev */
            unsigned char c = msd_buf[i];
            uart_putc((c >= 0x20 && c < 0x7f) ? (char)c : ' ');
        }
        uart_puts("'\n");
    }
    return st;
}
static int scsi_read_capacity(void)
{
    unsigned char cdb[10] = {0x25,0,0,0,0,0,0,0,0,0};   /* READ CAPACITY(10) */
    int st = bot_xfer(cdb, 10, msd_buf, 8, 1);
    if (st == 0) {
        unsigned int last = get32be(&msd_buf[0]);       /* last LBA */
        unsigned int bs   = get32be(&msd_buf[4]);       /* block size */
        msd_blocks = (unsigned long)last + 1UL;
        msd_bsize  = bs ? bs : 512;
    }
    return st;
}

/* Probe sequence: a removable unit typically answers TEST UNIT READY with
 * "not ready / becoming ready" a few times right after configuration, so poll
 * with REQUEST SENSE between tries before giving up. */
int usbmsd_init(void)
{
    msd_is_ready = 0;
    if (!xhci_msd_present()) { uart_puts("[usbmsd] no mass-storage device\n"); return -1; }

    int ready = 0;
    for (int i = 0; i < 30; i++) {
        int st = scsi_tur();
        if (st == 0) { ready = 1; break; }
        if (st < 0)  { delay_ms(20); continue; }   /* transport hiccup — retry */
        scsi_request_sense();                      /* clear the CHECK CONDITION */
        delay_ms(50);
    }
    if (!ready) { uart_puts("[usbmsd] unit never became ready\n"); return -1; }

    scsi_inquiry();

    if (scsi_read_capacity() != 0) { uart_puts("[usbmsd] READ CAPACITY failed\n"); return -1; }
    if (msd_bsize != 512) {
        uart_puts("[usbmsd] unsupported block size "); msd_dec(msd_bsize);
        uart_puts(" (only 512 supported)\n");
        return -1;
    }

    uart_puts("[usbmsd] ready: "); msd_dec(msd_blocks);
    uart_puts(" blocks x 512 B = "); msd_dec(msd_blocks / 2048);
    uart_puts(" MiB\n");
    msd_is_ready = 1;
    return 0;
}

int           usbmsd_ready(void)       { return msd_is_ready; }
unsigned long usbmsd_block_count(void) { return msd_blocks; }
unsigned int  usbmsd_block_size(void)  { return msd_bsize; }

int usbmsd_read_block(unsigned long lba, void *buf)
{
    if (!msd_is_ready && !xhci_msd_present()) return -1;
    unsigned char cdb[10];
    cdb[0]=0x28; cdb[1]=0;                                   /* READ(10) */
    cdb[2]=(unsigned char)(lba>>24); cdb[3]=(unsigned char)(lba>>16);  /* LBA, big-endian */
    cdb[4]=(unsigned char)(lba>>8);  cdb[5]=(unsigned char)lba;
    cdb[6]=0; cdb[7]=0; cdb[8]=1; cdb[9]=0;                  /* 1 block */
    int st = bot_xfer(cdb, 10, msd_buf, 512, 1);
    if (st != 0) return -1;
    unsigned char *d = (unsigned char *)buf;
    for (int i = 0; i < 512; i++) d[i] = msd_buf[i];
    return 0;
}

int usbmsd_write_block(unsigned long lba, const void *buf)
{
    if (!msd_is_ready && !xhci_msd_present()) return -1;
    const unsigned char *s = (const unsigned char *)buf;
    for (int i = 0; i < 512; i++) msd_buf[i] = s[i];
    unsigned char cdb[10];
    cdb[0]=0x2A; cdb[1]=0;                                   /* WRITE(10) */
    cdb[2]=(unsigned char)(lba>>24); cdb[3]=(unsigned char)(lba>>16);
    cdb[4]=(unsigned char)(lba>>8);  cdb[5]=(unsigned char)lba;
    cdb[6]=0; cdb[7]=0; cdb[8]=1; cdb[9]=0;                  /* 1 block */
    int st = bot_xfer(cdb, 10, msd_buf, 512, 0);
    return (st == 0) ? 0 : -1;
}
