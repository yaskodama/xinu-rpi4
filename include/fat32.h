// include/fat32.h — read-only FAT32 reader on top of sd_read_block.
//
// Single-partition mode: we expect the SD card's first partition to
// start at the LBA the MBR points to (typically LBA 8192 on a Pi OS
// card), and that partition to be FAT32 formatted.  We parse the
// BIOS Parameter Block in the first sector of the partition, then
// walk the root directory's cluster chain to enumerate every entry.
//
// All directory entries are reported to a visitor callback; the
// caller (loader/main.c) inserts them into the VFS under /sd/.
// Long-filename (LFN) entries are skipped, so users see the 8.3
// short name (e.g. CONFIG~1.TXT) — that's a deliberate trade-off
// for keeping this reader small.  Most Pi boot files have 8.3 names
// natively.

#ifndef XINU_RPI4_FAT32_H
#define XINU_RPI4_FAT32_H

/* Block-device hooks — a FAT32 volume can sit on the on-board EMMC2 microSD
 * (sd_read_block / sd_write_block) OR on a USB mass-storage device
 * (usbmsd_read_block / usbmsd_write_block).  fat32_mount() defaults to the
 * EMMC; fat32_mount_dev() lets the caller bind any 512-byte block device, so
 * one FAT32 reader/writer serves both /microsd and /sd. */
typedef int (*fat32_read_fn)(unsigned long lba, void *buf);
typedef int (*fat32_write_fn)(unsigned long lba, const void *buf);

/* Returned by fat32_mount() — opaque to callers but the struct is
 * exposed so they can keep a stack-allocated instance. */
typedef struct {
    unsigned int  bytes_per_sector;
    unsigned int  sectors_per_cluster;
    unsigned int  reserved_sectors;
    unsigned int  num_fats;
    unsigned int  sectors_per_fat;
    unsigned int  root_cluster;

    /* Derived: absolute LBA bases. */
    unsigned long part_lba;          /* first sector of the partition  */
    unsigned long fat_lba;            /* first FAT entry sector         */
    unsigned long data_lba;           /* cluster 2's first sector       */

    /* Bound block device (set by fat32_mount / fat32_mount_dev). */
    fat32_read_fn  rd;
    fat32_write_fn wr;
} fat32_t;

/* Read MBR to find the first partition, read its boot sector and
 * populate `fs`.  Returns 0 on success, -1 if anything is unexpected
 * (no MBR signature, partition isn't FAT32, etc.).  fat32_mount() binds the
 * on-board microSD (EMMC2); fat32_mount_dev() binds the given block device. */
int fat32_mount(fat32_t *fs);
int fat32_mount_dev(fat32_t *fs, fat32_read_fn rd, fat32_write_fn wr);

/* Visitor callback signature: called once per non-LFN, non-deleted
 * directory entry.  `is_dir` is non-zero if the entry is a sub-
 * directory.  `first_cluster` lets the caller recurse into a sub-
 * directory by calling fat32_walk_dir() again. */
typedef void (*fat32_visit_fn)(const char  *name,
                               int          is_dir,
                               unsigned long size,
                               unsigned int first_cluster,
                               int          depth,
                               void        *ctx);

/* Walk the directory whose first cluster is `cluster`, calling
 * `visit` for each entry.  `depth` is passed through verbatim so
 * the caller can produce indented output; for a top-level walk
 * pass 0.  Returns 0 on success. */
int fat32_walk_dir(fat32_t *fs, unsigned int cluster, int depth,
                   fat32_visit_fn visit, void *ctx);

/* Find a FREE data cluster's first-sector LBA (for the safe raw-write self-test
 * — writing there leaves the FAT/dir/files untouched).  0 ok, -1 if none. */
int fat32_find_free_lba(fat32_t *fs, unsigned long *lba);

/* Create a single-cluster file `name` (8.3) in the root directory with `len`
 * bytes of `data`.  Allocates a free cluster, marks it EOC in all FATs, and
 * appends a root-dir entry — only free clusters / free dir slots are touched.
 * Returns 0 ok, -1 I/O, -2 too big (>1 cluster), -3 no free cluster, -4 dir full. */
int fat32_create_file(fat32_t *fs, const char *name, const void *data, unsigned int len);

/* Read up to `max` bytes of the root-dir file `name` (8.3, first cluster only)
 * into `out`.  Returns the byte count read, or -1 if not found / I/O error. */
int fat32_read_file(fat32_t *fs, const char *name, void *out, unsigned int max);

/* Create OR overwrite a single-cluster root-dir file `name` with `len` bytes.
 * If it exists, its first cluster is rewritten in place and the dir-entry size
 * updated (no leak); else like fat32_create_file.  Returns 0 ok, -1 I/O, -2 too
 * big (>1 cluster), -3 no free cluster, -4 dir full. */
int fat32_write_file(fat32_t *fs, const char *name, const void *data, unsigned int len);

/* Read a WHOLE file (following its FAT cluster chain) up to `max` bytes — for
 * multi-cluster files like an .avm actor.  Read-only.  Returns bytes or -1. */
int fat32_read_file_full(fat32_t *fs, const char *name, void *out, unsigned int max);

/* Write a WHOLE file (multi-cluster), allocating + chaining free clusters.  If
 * `name` exists its old chain is freed and the dir entry reused.  Returns 0 ok. */
int fat32_write_file_full(fat32_t *fs, const char *name, const void *data, unsigned int len);

/* Resumable-read helpers (chunk a big file across frames for a load bar). */
int          fat32_open(fat32_t *fs, const char *name, unsigned int *first, unsigned int *size);
unsigned int fat32_next_cluster_of(fat32_t *fs, unsigned int cluster);
unsigned long fat32_cluster_lba(fat32_t *fs, unsigned int cluster);

#endif /* XINU_RPI4_FAT32_H */
