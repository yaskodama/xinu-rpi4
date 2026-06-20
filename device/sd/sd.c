// device/sd/sd.c — read-only SD block driver for the Pi 4 EMMC2 controller.
//
// The firmware boots kernel8.img off the µSD via EMMC2, but it does NOT reliably
// leave the controller in a state where a bare CMD17 just works once our kernel
// is running (verified: a minimal "assume hot" driver returns rc=ERR on LBA0).
// So we do the full SDHCI bring-up ourselves — reset the host, start the
// identify-speed clock, and enumerate the card (CMD0/8/ACMD41/CMD2/CMD3/CMD7/
// CMD16) before CMD17 reads.  This is the same sequence proven on the Pi 3
// Arasan EMMC (xinu-raz sd_block.c); EMMC2 on the Pi 4 is the same SDHCI core,
// just at SD_BASE = 0xFE340000.

#include "sd.h"

#ifdef SD_BASE

#define EMMC_ARG2          (*(volatile unsigned int *)(SD_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile unsigned int *)(SD_BASE + 0x04))
#define EMMC_ARG1          (*(volatile unsigned int *)(SD_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile unsigned int *)(SD_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile unsigned int *)(SD_BASE + 0x10))
#define EMMC_DATA          (*(volatile unsigned int *)(SD_BASE + 0x20))
#define EMMC_STATUS        (*(volatile unsigned int *)(SD_BASE + 0x24))
#define EMMC_CONTROL0      (*(volatile unsigned int *)(SD_BASE + 0x28))
#define EMMC_CONTROL1      (*(volatile unsigned int *)(SD_BASE + 0x2C))
#define EMMC_INTERRUPT     (*(volatile unsigned int *)(SD_BASE + 0x30))
#define EMMC_IRPT_MASK     (*(volatile unsigned int *)(SD_BASE + 0x34))
#define EMMC_IRPT_EN       (*(volatile unsigned int *)(SD_BASE + 0x38))

/* INTERRUPT register bits. */
#define INT_CMD_DONE       0x00000001
#define INT_DATA_DONE      0x00000002
#define INT_READ_RDY       0x00000020
#define INT_ERROR_MASK     0xFFFF8000u   /* any of bits 15..31 = error */

/* CONTROL1 bits. */
#define C1_CLK_INTLEN      0x00000001   /* internal clock enable            */
#define C1_CLK_STABLE      0x00000002
#define C1_CLK_EN          0x00000004   /* SD clock enable                  */
#define C1_SRST_HC         0x01000000   /* reset the complete host          */
#define C1_SRST_CMD        0x02000000   /* reset the command line           */
#define C1_SRST_DAT        0x04000000   /* reset the data line              */

/* Command + transfer-mode words (index<<24 | flags). */
#define CMD_GO_IDLE        0x00000000   /* CMD0,  no response               */
#define CMD_SEND_IF_COND   0x08020000   /* CMD8,  R7                        */
#define CMD_APP_CMD        0x37020000   /* CMD55, R1                        */
#define CMD_SD_SENDOPCOND  0x29020000   /* ACMD41,R3 (no CRC/idx check)     */
#define CMD_ALL_SEND_CID   0x02010000   /* CMD2,  R2 (136-bit)              */
#define CMD_SEND_REL_ADDR  0x03020000   /* CMD3,  R6                        */
#define CMD_SELECT_CARD    0x07030000   /* CMD7,  R1b                       */
#define CMD_SET_BLOCKLEN   0x10020000   /* CMD16, R1                        */
#define CMD_READ_SINGLE    0x113A0010   /* CMD17, R1 + data card->host      */
#define CMD_WRITE_SINGLE   0x183A0000   /* CMD24, R1 + data host->card      */
#define INT_WRITE_RDY      0x00000010   /* buffer write-ready (bit 4)       */

#define POLL_LIMIT         3000000UL

/* STATUS inhibit bits: CMD / DAT line busy. */
#define ST_CMD_INHIBIT     0x00000001
#define ST_DAT_INHIBIT     0x00000002

static int sd_inited;
static volatile int sd_dbg_step;
int sd_debug_step(void) { return sd_dbg_step; }

/* Busy-wait `us` microseconds off the architectural counter (CNTPCT_EL0).  The
 * previous code used bare empty `for` loops for the power/clock settle, which
 * (a) -O2 can delete entirely and (b) were sized for the Pi 3's faster identify
 * clock.  On the Pi 4 EMMC2 the base clock is 100 MHz and the identify divider
 * (0x200) gives SDCLK ≈ 98 kHz, so the card's mandatory 74-clock power-up window
 * is ~755 µs — far longer than those loops delivered, so CMD0/CMD8 were issued
 * before the card was ready and timed out.  A real, un-optimisable delay fixes it
 * (confirmed live via /mmio-write: with adequate settle, CMD8 echoes 0x1AA and
 * ACMD41 reaches OCR 0xC0FF8000 SDHC-ready). */
static void sd_udelay(unsigned us)
{
    unsigned long freq, start, now;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
    unsigned long target = ((unsigned long)us * freq) / 1000000UL;
    do { __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now)); } while (now - start < target);
}

static volatile unsigned int sd_dbg_int;   /* INTERRUPT captured at the last failure */
unsigned int sd_last_int(void) { return sd_dbg_int; }   /* last failure's INTERRUPT bits */

/* Snapshot the key controller registers for the `mount` diagnostic.  `intr` is
 * the INTERRUPT value captured AT the failing command (wait_int clears the live
 * register w1c), so its error bits survive (bit16=CTO cmd-timeout, 17=CCRC,
 * 18=CEND, 19=CIDX, 20=DTO data-timeout, 21=DCRC, 22=DEND). */
void sd_debug_regs(unsigned int *ctrl0, unsigned int *ctrl1, unsigned int *status,
                   unsigned int *intr, unsigned int *resp0)
{
    if (ctrl0)  *ctrl0  = EMMC_CONTROL0;
    if (ctrl1)  *ctrl1  = EMMC_CONTROL1;
    if (status) *status = EMMC_STATUS;
    if (intr)   *intr   = sd_dbg_int;
    if (resp0)  *resp0  = EMMC_RESP0;
}

static int wait_int(unsigned int mask)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++) {
        unsigned int v = EMMC_INTERRUPT;
        if (v & INT_ERROR_MASK) { sd_dbg_int = v; EMMC_INTERRUPT = v; return -1; }
        if (v & mask)           { EMMC_INTERRUPT = mask; return 0; }
    }
    sd_dbg_int = EMMC_INTERRUPT;   /* plain timeout: capture whatever is set */
    return -1;
}

/* Spin until the controller will accept a new command (prior command's CMD and
 * DAT lines free).  Issuing back-to-back commands without this drops the second
 * — the classic "every read fails past CMD8" bug. */
static int sd_wait_ready(void)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_STATUS & (ST_CMD_INHIBIT | ST_DAT_INHIBIT))) return 0;
    return -1;
}

/* Issue a command (no data phase) and wait for completion. */
static int sd_cmd(unsigned int cmdtm, unsigned int arg)
{
    if (sd_wait_ready() != 0) return -1;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_ARG1 = arg;
    EMMC_CMDTM = cmdtm;
    return wait_int(INT_CMD_DONE);
}

int sd_init(void)
{
    unsigned long t;
    unsigned int  rca;

    if (sd_inited) return 0;

    /* The Pi 4 microSD CMD8 bring-up is settle-sensitive and intermittently
     * times out (CTO) on a cold boot.  Retry the WHOLE bring-up several times,
     * lengthening the power/clock settle each pass, so a flaky cold card still
     * comes up reliably instead of leaving /microsd unreadable for that boot. */
    int ok = 0;
    for (int attempt = 0; attempt < 6 && !ok; attempt++) {
        unsigned int settle = 10000u + (unsigned int)attempt * 15000u;  /* 10..85 ms */

        /* Reset the whole host, wait for the reset bit to self-clear. */
        EMMC_CONTROL1 = C1_SRST_HC;
        for (t = 0; t < POLL_LIMIT; t++)
            if (!(EMMC_CONTROL1 & C1_SRST_HC)) break;
        if (EMMC_CONTROL1 & C1_SRST_HC) { sd_dbg_step = 1; continue; }

        /* Identify-speed clock (≈98 kHz), wait stable, enable SD clock. */
        EMMC_CONTROL1 = 0x000E0081u;
        for (t = 0; t < POLL_LIMIT; t++)
            if (EMMC_CONTROL1 & C1_CLK_STABLE) break;
        if (!(EMMC_CONTROL1 & C1_CLK_STABLE)) { sd_dbg_step = 12; continue; }
        EMMC_CONTROL1 |= C1_CLK_EN;

        /* Bus power ON + 3.3 V select. */
        EMMC_CONTROL0 = (EMMC_CONTROL0 & ~0x00000F00u) | 0x00000F00u;

        /* Power/clock settle before the first command (lengthened per retry). */
        sd_udelay(settle);

        EMMC_IRPT_EN   = 0;
        EMMC_IRPT_MASK = 0xFFFFFFFFu;
        EMMC_INTERRUPT = 0xFFFFFFFFu;
        sd_dbg_step = 2;

        sd_cmd(CMD_GO_IDLE, 0);                            /* CMD0 (no response) */
        sd_udelay(2000);                                   /* reach idle         */
        if (sd_cmd(CMD_SEND_IF_COND, 0x1AA) != 0) { sd_dbg_step = 40; continue; }  /* CMD8 */
        if (EMMC_RESP0 != 0x1AA)                  { sd_dbg_step = 41; continue; }

        /* ACMD41 until the OCR busy bit (31) sets. */
        int powered = 0;
        for (t = 0; t < 1000; t++) {
            if (sd_cmd(CMD_APP_CMD, 0) != 0)                 { sd_dbg_step = 50; break; }
            if (sd_cmd(CMD_SD_SENDOPCOND, 0x40FF8000u) != 0) { sd_dbg_step = 60; break; }
            if (EMMC_RESP0 & 0x80000000u) { powered = 1; break; }
            sd_udelay(1000);
        }
        if (!powered) { sd_dbg_step = 61; continue; }

        if (sd_cmd(CMD_ALL_SEND_CID, 0) != 0)  { sd_dbg_step = 80;  continue; }  /* CMD2 */
        if (sd_cmd(CMD_SEND_REL_ADDR, 0) != 0) { sd_dbg_step = 90;  continue; }  /* CMD3 */
        rca = EMMC_RESP0 & 0xFFFF0000u;
        if (sd_cmd(CMD_SELECT_CARD, rca) != 0) { sd_dbg_step = 100; continue; }  /* CMD7 */
        if (sd_cmd(CMD_SET_BLOCKLEN, SD_BLOCK_SIZE) != 0) { sd_dbg_step = 110; continue; } /* CMD16 */

        ok = 1;
    }
    if (!ok) return -1;

    sd_dbg_step = 11;
    sd_inited = 1;
    return 0;
}

/* Reset the CMD + DAT lines after an error so the controller isn't left wedged
 * (a CTO with no recovery makes EVERY following command time out too). */
static void sd_reset_cmd_dat(void)
{
    EMMC_CONTROL1 |= (C1_SRST_CMD | C1_SRST_DAT);
    for (unsigned long t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_CONTROL1 & (C1_SRST_CMD | C1_SRST_DAT))) break;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
}

int sd_read_block(unsigned long lba, void *buf)
{
    unsigned int *p = (unsigned int *)buf;

    if (!sd_inited && sd_init() != 0) return -1;

    /* Retry with a CMD/DAT-line reset between attempts: recovers transient CMD
     * timeouts (CTO) AND, on final failure, leaves the lines reset so the next
     * read (e.g. the directory scan) still works instead of staying wedged. */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (sd_wait_ready() != 0)            { sd_reset_cmd_dat(); continue; }
        EMMC_INTERRUPT  = 0xFFFFFFFFu;
        EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;   /* 1 block of 512 B */
        EMMC_ARG1       = (unsigned int)lba;            /* SDHC: block address */
        EMMC_CMDTM      = CMD_READ_SINGLE;              /* CMD17 */
        if (wait_int(INT_CMD_DONE) != 0)     { sd_reset_cmd_dat(); continue; }
        if (wait_int(INT_READ_RDY) != 0)     { sd_reset_cmd_dat(); continue; }
        for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) p[i] = EMMC_DATA;
        if (wait_int(INT_DATA_DONE) != 0)    { sd_reset_cmd_dat(); continue; }
        return 0;
    }
    return -1;
}

int sd_write_block(unsigned long lba, const void *buf)
{
    const unsigned int *p = (const unsigned int *)buf;

    if (!sd_inited && sd_init() != 0) return -1;

    if (sd_wait_ready() != 0) return -1;
    EMMC_INTERRUPT  = 0xFFFFFFFFu;
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;      /* 1 block of 512 B    */
    EMMC_ARG1       = (unsigned int)lba;               /* SDHC: block address */
    EMMC_CMDTM      = CMD_WRITE_SINGLE;                /* CMD24               */

    if (wait_int(INT_CMD_DONE)  != 0) return -1;
    if (wait_int(INT_WRITE_RDY) != 0) return -1;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) EMMC_DATA = p[i];
    if (wait_int(INT_DATA_DONE) != 0) return -1;
    return 0;
}

#else /* !SD_BASE — QEMU: no controller we can talk to */

int sd_init(void)                                 { return -1; }
int sd_read_block(unsigned long lba, void *buf)   { (void)lba; (void)buf; return -1; }
int sd_write_block(unsigned long lba, const void *buf) { (void)lba; (void)buf; return -1; }
int sd_debug_step(void)                           { return 0; }

#endif
