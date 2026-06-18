// device/sd/sd.c — read-only SDHCI block driver.
//
// Bare minimum to talk to a pre-initialised Pi 4 EMMC2 controller.
// We never touch the controller's clocks, power, or command-init
// sequence — the firmware bootloader handled all that to read
// kernel8.img off the card.  Issuing CMD17 against an already-
// selected card with block addressing in effect just works (as long
// as no other code has messed with the controller in between).

#include "sd.h"

#ifdef SD_BASE

/* SDHCI v3 register layout (offsets from SD_BASE).  Reference:
 * Broadcom BCM2711 ARM peripherals chapter 5 "External Mass Media
 * Controller", + the standard SDHCI spec for shared register meaning. */
#define EMMC_ARG2          (*(volatile unsigned int *)(SD_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile unsigned int *)(SD_BASE + 0x04))
#define EMMC_ARG1          (*(volatile unsigned int *)(SD_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile unsigned int *)(SD_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile unsigned int *)(SD_BASE + 0x10))
#define EMMC_DATA          (*(volatile unsigned int *)(SD_BASE + 0x20))
#define EMMC_STATUS        (*(volatile unsigned int *)(SD_BASE + 0x24))
#define EMMC_INTERRUPT     (*(volatile unsigned int *)(SD_BASE + 0x30))
#define EMMC_IRPT_MASK     (*(volatile unsigned int *)(SD_BASE + 0x34))  /* status-latch enable */
#define EMMC_IRPT_EN       (*(volatile unsigned int *)(SD_BASE + 0x38))  /* IRQ-signal enable   */

/* STATUS register inhibit bits: a command must not be issued while either
 * the command or data lines are still busy with a prior transaction. */
#define STATUS_CMD_INHIBIT (1u << 0)
#define STATUS_DAT_INHIBIT (1u << 1)

/* CMDTM bit fields per SDHCI spec.
 *   bits 24-31 = command index
 *   bit  21    = data direction is data (ISDATA)
 *   bit  20    = check response index against CMD index
 *   bit  19    = check response CRC7
 *   bits 16-17 = response type (10 = 48 bit, 01 = 136 bit, 00 = none)
 *   bit  4     = transfer direction (0 = host->card, 1 = card->host)
 */
#define CMDTM_CMD17_READ_SINGLE_BLOCK \
    ((17u << 24) | (1u << 21) | (1u << 20) | (1u << 19) | (2u << 16) | (1u << 4))

/* INTERRUPT register bits we care about (low half is normal events,
 * high half is errors). */
#define INT_CMD_DONE       (1u << 0)
#define INT_DATA_DONE      (1u << 1)
#define INT_READ_RDY       (1u << 5)
#define INT_ERROR_MASK     0xFFFF8000u   /* any of bits 15..31 = error */

/* Spin budgets — generous enough for a slow card but bounded so a
 * dead controller doesn't hang the boot forever. */
#define POLL_LIMIT         5000000UL

static int wait_intr(unsigned int flag)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++) {
        unsigned int r = EMMC_INTERRUPT;
        if (r & INT_ERROR_MASK) return -1;
        if (r & flag) {
            EMMC_INTERRUPT = flag;   /* w1c — clear the bit we waited on */
            return 0;
        }
    }
    return -1;
}

/* Wait until the controller will accept a new command — both the CMD and DAT
 * lines must be idle (inhibit bits clear).  Skipping this is the classic
 * intermittent-read bug: CMD17 issued while the previous transfer still holds
 * the bus is silently dropped and the read times out. */
static int wait_ready(void)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_STATUS & (STATUS_CMD_INHIBIT | STATUS_DAT_INHIBIT)))
            return 0;
    return -1;
}

int sd_init(void)
{
    /* The firmware left EMMC2 clocked and the card selected with block
     * addressing (it just read kernel8.img off it).  We only need to make
     * the controller's interrupt-status bits actually LATCH: without
     * IRPT_MASK enabling them, the INTERRUPT register never sets CMD_DONE /
     * READ_RDY and every wait_intr() times out.  Leave the IRQ *signal*
     * (IRPT_EN) off — we poll, we don't take SD interrupts. */
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_IRPT_EN   = 0u;
    EMMC_INTERRUPT = 0xFFFFFFFFu;   /* clear any stale status */

    /* Sanity check: try reading LBA 0.  If that comes back with the
     * usual MBR signature (0x55 0xAA at offset 510) we're good. */
    unsigned char block[SD_BLOCK_SIZE];
    if (sd_read_block(0, block) != 0) return -1;
    if (block[510] != 0x55 || block[511] != 0xAA) return -1;
    return 0;
}

int sd_read_block(unsigned long lba, void *buf)
{
    /* Don't issue CMD17 until the bus is idle. */
    if (wait_ready() != 0) return -1;

    /* Clear any stale interrupt status from the previous transaction. */
    EMMC_INTERRUPT = 0xFFFFFFFFu;

    /* One block of 512 bytes. */
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;

    /* SDHC/SDXC cards (which Pi firmware leaves the EMMC speaking to)
     * use block addressing — the ARG is the LBA directly, not a byte
     * offset.  Block address >32 bits is unusual but Pi 4 supports it. */
    EMMC_ARG1 = (unsigned int)lba;

    /* Issue CMD17. */
    EMMC_CMDTM = CMDTM_CMD17_READ_SINGLE_BLOCK;

    /* Wait for the command to be accepted. */
    if (wait_intr(INT_CMD_DONE) != 0) return -1;

    /* Wait for the data buffer to fill. */
    if (wait_intr(INT_READ_RDY) != 0) return -1;

    /* Drain 128 32-bit words from the data port. */
    unsigned int *p = (unsigned int *)buf;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) {
        p[i] = EMMC_DATA;
    }

    /* Wait for the controller to acknowledge end of transfer. */
    if (wait_intr(INT_DATA_DONE) != 0) return -1;

    return 0;
}

#else /* !SD_BASE — QEMU: no controller we can talk to */

int sd_init(void)                                 { return -1; }
int sd_read_block(unsigned long lba, void *buf)   { (void)lba; (void)buf; return -1; }

#endif
