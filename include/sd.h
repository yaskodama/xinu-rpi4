// include/sd.h — minimal SD/MMC block reader.
//
// On Pi 4 (BCM2711) the second EMMC controller (EMMC2, Arasan SDHCI
// compatible) sits at SD_BASE = 0xFE340000 and is what the firmware
// uses to boot from the µSD card.  By the time our kernel runs the
// firmware has already:
//   1. Powered up the controller and selected EMMC2 for GPIO 48..53
//      (the SD card slot lines on Pi 4).
//   2. Sent CMD0/CMD8/ACMD41/CMD2/CMD3/CMD7 to bring the card into
//      "transfer" state with block addressing.
//   3. Read kernel8.img and any other files it needed from FAT32.
//
// So we can do read-only block accesses without doing the full SDHCI
// initialisation sequence ourselves — just issue CMD17 (read single
// block) and pull 128 little-endian words from the data port.
//
// On QEMU virt there's no SDHCI at all.  Builds that don't define
// SD_BASE turn sd_init() into "return -1" and sd_read_block() into a
// no-op, so the higher-level mount code drops cleanly to
// "no /sd available".

#ifndef XINU_RPI4_SD_H
#define XINU_RPI4_SD_H

#define SD_BLOCK_SIZE  512

/* Probe the controller / verify a sane state.  Returns 0 if
 * sd_read_block() should work, -1 otherwise (SD_BASE undefined,
 * controller absent, or a sanity-check read of LBA 0 failed). */
int sd_init(void);

/* Read one 512-byte block at logical block address `lba` into `buf`
 * (which must be at least 512 bytes).  Returns 0 on success, -1 on
 * timeout / hardware error. */
int sd_read_block(unsigned long lba, void *buf);

/* Write one 512-byte block at `lba` from `buf` (CMD24).  Returns 0 on success.
 * Caller is responsible for filesystem consistency — this is a raw block write. */
int sd_write_block(unsigned long lba, const void *buf);

/* Last init progress marker (diagnostics).  Stages: 2=reset+clock done,
 * 40=CMD8 issue fail, 41=CMD8 bad echo, 61=ACMD41 never ready, 80=CMD2,
 * 90=CMD3, 100=CMD7, 110=CMD16, 11=fully initialised. */
int sd_debug_step(void);

/* Snapshot CONTROL0 / CONTROL1 / STATUS / INTERRUPT(at last failure) / RESP0. */
void sd_debug_regs(unsigned int *ctrl0, unsigned int *ctrl1, unsigned int *status,
                   unsigned int *intr, unsigned int *resp0);

#endif /* XINU_RPI4_SD_H */
