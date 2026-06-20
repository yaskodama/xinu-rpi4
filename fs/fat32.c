// fs/fat32.c — read-only FAT32 reader.

#include "fat32.h"
#include "sd.h"

/* Little-endian unpackers — FAT32 metadata is always LE on disk. */
static unsigned int le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* One sector worth of scratch — re-used by directory and FAT walks
 * to keep stack pressure low. */
static unsigned char scratch[SD_BLOCK_SIZE];

/* Bind the on-board microSD (EMMC2) and mount its first FAT32 partition. */
int fat32_mount(fat32_t *fs)
{
    return fat32_mount_dev(fs, sd_read_block, sd_write_block);
}

/* Bind an arbitrary 512-byte block device (microSD or USB MSD) and mount its
 * first FAT32 partition.  All later fat32_* calls reuse fs->rd / fs->wr. */
int fat32_mount_dev(fat32_t *fs, fat32_read_fn rd, fat32_write_fn wr)
{
    fs->rd = rd;
    fs->wr = wr;

    unsigned char mbr[SD_BLOCK_SIZE];
    if (fs->rd(0, mbr) != 0)                    return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)   return -1;

    /* First partition table entry starts at MBR offset 0x1BE.
     *   +0x04 = partition type byte (0x0B / 0x0C for FAT32)
     *   +0x08 = LBA of first sector (LE u32) */
    unsigned char ptype = mbr[0x1BE + 0x04];
    if (ptype != 0x0B && ptype != 0x0C)         return -1;

    fs->part_lba = le32(&mbr[0x1BE + 0x08]);

    /* Read the partition's boot sector to get the BPB. */
    if (fs->rd(fs->part_lba, scratch) != 0) return -1;
    if (scratch[510] != 0x55 || scratch[511] != 0xAA) return -1;

    fs->bytes_per_sector   = le16(&scratch[0x0B]);
    fs->sectors_per_cluster= scratch[0x0D];
    fs->reserved_sectors   = le16(&scratch[0x0E]);
    fs->num_fats           = scratch[0x10];
    fs->sectors_per_fat    = le32(&scratch[0x24]);
    fs->root_cluster       = le32(&scratch[0x2C]);

    if (fs->bytes_per_sector  != SD_BLOCK_SIZE) return -1;
    if (fs->sectors_per_cluster == 0)           return -1;
    if (fs->root_cluster < 2)                    return -1;

    fs->fat_lba  = fs->part_lba + fs->reserved_sectors;
    fs->data_lba = fs->fat_lba
                 + (unsigned long)fs->num_fats * fs->sectors_per_fat;

    return 0;
}

/* Translate a cluster number to the LBA of its first sector. */
static unsigned long cluster_to_lba(const fat32_t *fs, unsigned int cluster)
{
    return fs->data_lba
         + (unsigned long)(cluster - 2) * fs->sectors_per_cluster;
}

/* Find a FREE data cluster (FAT entry == 0) and return its first sector's LBA
 * in *lba.  A free cluster is referenced by no file and marked free in the FAT,
 * so writing to it (without touching the FAT/dir) leaves the filesystem intact
 * — used by the safe SD write self-test.  Returns 0 on success, -1 if none
 * found within the scanned range. */
int fat32_find_free_lba(fat32_t *fs, unsigned long *lba)
{
    unsigned int cluster = 2;
    for (unsigned int s = 0; s < 2048; s++) {          /* scan up to 2048 FAT sectors */
        if (fs->rd(fs->fat_lba + s, scratch) != 0) return -1;
        for (int e = 0; e < SD_BLOCK_SIZE / 4; e++, cluster++) {
            if ((le32(&scratch[e * 4]) & 0x0FFFFFFFu) == 0) {
                *lba = cluster_to_lba(fs, cluster);
                return 0;
            }
        }
    }
    return -1;
}

/* Read the FAT entry for `cluster`.  Returns the next-cluster value;
 * >= 0x0FFFFFF8 means end of chain.  Returns 0x0FFFFFFF on read
 * error so the caller terminates the walk. */
static unsigned int fat32_next_cluster(fat32_t *fs, unsigned int cluster)
{
    unsigned long fat_byte = (unsigned long)cluster * 4UL;
    unsigned long sec = fs->fat_lba + (fat_byte / SD_BLOCK_SIZE);
    unsigned int  off = (unsigned int)(fat_byte % SD_BLOCK_SIZE);

    if (fs->rd(sec, scratch) != 0) return 0x0FFFFFFFu;
    unsigned int v = le32(&scratch[off]) & 0x0FFFFFFFu;
    return v;
}

/* Format an 8.3 directory entry name into a NUL-terminated C string.
 * Trailing spaces are trimmed; a `.` is inserted between base and
 * extension if the extension is non-empty. */
static void format_8_3(const unsigned char *raw, char *out, int outlen)
{
    int o = 0;
    /* 8-char base name */
    for (int i = 0; i < 8 && o < outlen - 1; i++) {
        if (raw[i] == ' ') break;
        out[o++] = (char)raw[i];
    }
    /* extension */
    int ext_present = 0;
    for (int i = 8; i < 11; i++) {
        if (raw[i] != ' ') { ext_present = 1; break; }
    }
    if (ext_present && o < outlen - 1) out[o++] = '.';
    for (int i = 8; i < 11 && o < outlen - 1; i++) {
        if (raw[i] == ' ') break;
        out[o++] = (char)raw[i];
    }
    out[o] = 0;
}

int fat32_walk_dir(fat32_t *fs, unsigned int cluster, int depth,
                   fat32_visit_fn visit, void *ctx)
{
    unsigned char dirsec[SD_BLOCK_SIZE];
    unsigned int  cur = cluster;
    int           safety = 1024;  /* bound the cluster-chain walk    */

    while (cur >= 2 && cur < 0x0FFFFFF8u && safety-- > 0) {
        unsigned long base = cluster_to_lba(fs, cur);
        for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
            if (fs->rd(base + s, dirsec) != 0) return -1;

            for (unsigned int off = 0; off + 32 <= SD_BLOCK_SIZE; off += 32) {
                unsigned char *e = &dirsec[off];

                if (e[0] == 0x00) return 0;   /* end-of-directory marker */
                if (e[0] == 0xE5) continue;   /* deleted entry           */
                if ((e[11] & 0x0F) == 0x0F) continue;  /* LFN chunk      */
                if (e[11] & 0x08) continue;   /* volume label            */

                char name[16];
                format_8_3(e, name, sizeof name);

                /* Skip "." and ".." pseudo-entries to avoid loops. */
                if (name[0] == '.' && (name[1] == 0
                    || (name[1] == '.' && name[2] == 0))) continue;

                int           is_dir = (e[11] & 0x10) ? 1 : 0;
                unsigned long size   = le32(&e[28]);
                unsigned int  first  = (le16(&e[20]) << 16) | le16(&e[26]);

                visit(name, is_dir, size, first, depth, ctx);
            }
        }
        cur = fat32_next_cluster(fs, cur);
    }
    return 0;
}

/* ---- minimal FAT32 WRITE-BACK: create a single-cluster file ----------------
 * Allocates ONE free cluster, writes `data` (zero-padded), marks the cluster
 * EOC in every FAT copy, and appends a directory entry to the root directory's
 * first free slot.  Only free clusters / free dir slots are touched — existing
 * files and the FAT chains they use are never modified.  Single cluster only,
 * so len must fit one cluster (plenty for a text file).  8.3 names only.
 * Returns 0 ok, -1 I/O error, -2 too big, -3 no free cluster, -4 dir full. */
static unsigned char up8(unsigned char c)
{
    if (c >= 'a' && c <= 'z') return (unsigned char)(c - 'a' + 'A');
    return c;
}

int fat32_create_file(fat32_t *fs, const char *name,
                      const void *data, unsigned int len)
{
    unsigned int cluster_bytes = (unsigned int)fs->sectors_per_cluster * SD_BLOCK_SIZE;
    if (len > cluster_bytes) return -2;

    /* 1. find a free cluster (FAT entry == 0). */
    unsigned int fc = 0, cluster = 2;
    int found = 0;
    for (unsigned int s = 0; s < 2048 && !found; s++) {
        if (fs->rd(fs->fat_lba + s, scratch) != 0) return -1;
        for (int e = 0; e < SD_BLOCK_SIZE / 4; e++, cluster++)
            if ((le32(&scratch[e * 4]) & 0x0FFFFFFFu) == 0) { fc = cluster; found = 1; break; }
    }
    if (!found) return -3;

    /* 2. write the data into the cluster (zero-padded, per sector). */
    {
        unsigned long base = cluster_to_lba(fs, fc);
        unsigned int nsec = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
        if (nsec == 0) nsec = 1;
        const unsigned char *d = (const unsigned char *)data;
        for (unsigned int s = 0; s < nsec; s++) {
            unsigned char buf[SD_BLOCK_SIZE];
            for (int i = 0; i < SD_BLOCK_SIZE; i++) {
                unsigned int idx = s * SD_BLOCK_SIZE + (unsigned int)i;
                buf[i] = (idx < len) ? d[idx] : 0;
            }
            if (fs->wr(base + s, buf) != 0) return -1;
        }
    }

    /* 3. mark FAT[fc] = EOC (0x0FFFFFF8) in every FAT copy. */
    {
        unsigned long fat_byte = (unsigned long)fc * 4UL;
        unsigned long secoff = fat_byte / SD_BLOCK_SIZE;
        unsigned int  off    = (unsigned int)(fat_byte % SD_BLOCK_SIZE);
        for (unsigned int f = 0; f < fs->num_fats; f++) {
            unsigned long sec = fs->fat_lba + (unsigned long)f * fs->sectors_per_fat + secoff;
            if (fs->rd(sec, scratch) != 0) return -1;
            scratch[off + 0] = 0xF8; scratch[off + 1] = 0xFF;
            scratch[off + 2] = 0xFF; scratch[off + 3] = 0x0F;
            if (fs->wr(sec, scratch) != 0) return -1;
        }
    }

    /* 4. build the 8.3 directory entry. */
    unsigned char ent[32];
    for (int i = 0; i < 11; i++) ent[i] = ' ';
    {
        int i = 0, o = 0;
        for (; name[i] && name[i] != '.' && o < 8; i++) ent[o++] = up8((unsigned char)name[i]);
        while (name[i] && name[i] != '.') i++;
        if (name[i] == '.') { i++; int e = 8; for (; name[i] && e < 11; i++) ent[e++] = up8((unsigned char)name[i]); }
    }
    ent[11] = 0x20;                                   /* archive */
    for (int k = 12; k < 32; k++) ent[k] = 0;
    ent[20] = (unsigned char)(fc >> 16); ent[21] = (unsigned char)(fc >> 24);  /* cluster hi */
    ent[26] = (unsigned char)(fc);       ent[27] = (unsigned char)(fc >> 8);   /* cluster lo */
    ent[28] = (unsigned char)(len);       ent[29] = (unsigned char)(len >> 8);
    ent[30] = (unsigned char)(len >> 16); ent[31] = (unsigned char)(len >> 24);

    /* 5. append into the root directory's first free (0x00 / 0xE5) slot. */
    {
        unsigned int cur = fs->root_cluster;
        int safety = 1024;
        while (cur >= 2 && cur < 0x0FFFFFF8u && safety-- > 0) {
            unsigned long cbase = cluster_to_lba(fs, cur);
            for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
                if (fs->rd(cbase + s, scratch) != 0) return -1;
                for (int off2 = 0; off2 + 32 <= SD_BLOCK_SIZE; off2 += 32) {
                    unsigned char c0 = scratch[off2];
                    if (c0 == 0x00 || c0 == 0xE5) {
                        for (int k = 0; k < 32; k++) scratch[off2 + k] = ent[k];
                        if (fs->wr(cbase + s, scratch) != 0) return -1;
                        return 0;
                    }
                }
            }
            cur = fat32_next_cluster(fs, cur);
        }
    }
    return -4;   /* no free directory slot */
}

/* ---- read / overwrite a single-cluster file (used for small config files) -- */

/* Build the raw 11-byte 8.3 name (space-padded, upper-cased) from a C string. */
static void name_to_83(const char *name, unsigned char out[11])
{
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    for (; name[i] && name[i] != '.' && o < 8; i++) out[o++] = up8((unsigned char)name[i]);
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; int e = 8; for (; name[i] && e < 11; i++) out[e++] = up8((unsigned char)name[i]); }
}

/* Locate `name83` in the root directory.  On success returns 0 and fills the
 * on-disk location of its 32-byte entry (*ent_lba sector + *ent_off byte offset)
 * plus its first cluster and size.  Returns -1 if not found / I/O error. */
static int find_in_root(fat32_t *fs, const unsigned char name83[11],
                        unsigned long *ent_lba, unsigned int *ent_off,
                        unsigned int *first_cluster, unsigned int *size)
{
    unsigned char dirsec[SD_BLOCK_SIZE];
    unsigned int  cur = fs->root_cluster;
    int           safety = 1024;
    while (cur >= 2 && cur < 0x0FFFFFF8u && safety-- > 0) {
        unsigned long base = cluster_to_lba(fs, cur);
        for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
            if (fs->rd(base + s, dirsec) != 0) return -1;
            for (unsigned int off = 0; off + 32 <= SD_BLOCK_SIZE; off += 32) {
                unsigned char *e = &dirsec[off];
                if (e[0] == 0x00) return -1;            /* end of directory      */
                if (e[0] == 0xE5) continue;             /* deleted               */
                if ((e[11] & 0x0F) == 0x0F) continue;   /* LFN chunk             */
                if (e[11] & 0x08) continue;             /* volume label          */
                int match = 1;
                for (int k = 0; k < 11; k++) if (e[k] != name83[k]) { match = 0; break; }
                if (!match) continue;
                if (ent_lba) *ent_lba = base + s;
                if (ent_off) *ent_off = off;
                if (first_cluster) *first_cluster = (le16(&e[20]) << 16) | le16(&e[26]);
                if (size) *size = le32(&e[28]);
                return 0;
            }
        }
        cur = fat32_next_cluster(fs, cur);
    }
    return -1;
}

/* Read up to `max` bytes of the root-dir file `name` (8.3) into `out`.  Reads
 * only the file's first cluster (these helpers only ever write single-cluster
 * files), capped at `max` and the file size.  Returns the byte count, or -1 if
 * the file does not exist / on I/O error. */
int fat32_read_file(fat32_t *fs, const char *name, void *out, unsigned int max)
{
    unsigned char name83[11];
    unsigned int first = 0, size = 0;
    name_to_83(name, name83);
    if (find_in_root(fs, name83, 0, 0, &first, &size) != 0) return -1;
    if (first < 2) return -1;

    unsigned int cluster_bytes = (unsigned int)fs->sectors_per_cluster * SD_BLOCK_SIZE;
    unsigned int n = size;
    if (n > max)           n = max;
    if (n > cluster_bytes) n = cluster_bytes;

    unsigned long base = cluster_to_lba(fs, first);
    unsigned char *o = (unsigned char *)out;
    unsigned int nsec = (n + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    unsigned char buf[SD_BLOCK_SIZE];
    for (unsigned int s = 0; s < nsec; s++) {
        if (fs->rd(base + s, buf) != 0) return -1;
        unsigned int got = n - s * SD_BLOCK_SIZE;
        if (got > SD_BLOCK_SIZE) got = SD_BLOCK_SIZE;
        for (unsigned int i = 0; i < got; i++) o[s * SD_BLOCK_SIZE + i] = buf[i];
    }
    return (int)n;
}

/* Create OR overwrite a single-cluster root-dir file.  If `name` already exists
 * its existing first cluster is rewritten in place (zero-padded) and the dir
 * entry's size field updated — no cluster/dir leak — so a config file can be
 * re-saved repeatedly.  Otherwise behaves like fat32_create_file.  Assumes the
 * existing file is single-cluster (true for files these helpers create).
 * Returns 0 ok, -1 I/O, -2 too big, -3 no free cluster, -4 dir full. */
int fat32_write_file(fat32_t *fs, const char *name, const void *data, unsigned int len)
{
    unsigned int cluster_bytes = (unsigned int)fs->sectors_per_cluster * SD_BLOCK_SIZE;
    if (len > cluster_bytes) return -2;

    unsigned char name83[11];
    unsigned long ent_lba = 0; unsigned int ent_off = 0, first = 0;
    name_to_83(name, name83);
    if (find_in_root(fs, name83, &ent_lba, &ent_off, &first, 0) != 0 || first < 2)
        return fat32_create_file(fs, name, data, len);   /* new file */

    /* Overwrite the existing cluster (zero-padded), then patch the dir size. */
    unsigned long base = cluster_to_lba(fs, first);
    unsigned int nsec = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    if (nsec == 0) nsec = 1;
    const unsigned char *d = (const unsigned char *)data;
    for (unsigned int s = 0; s < nsec; s++) {
        unsigned char buf[SD_BLOCK_SIZE];
        for (int i = 0; i < SD_BLOCK_SIZE; i++) {
            unsigned int idx = s * SD_BLOCK_SIZE + (unsigned int)i;
            buf[i] = (idx < len) ? d[idx] : 0;
        }
        if (fs->wr(base + s, buf) != 0) return -1;
    }
    if (fs->rd(ent_lba, scratch) != 0) return -1;
    scratch[ent_off + 28] = (unsigned char)(len);
    scratch[ent_off + 29] = (unsigned char)(len >> 8);
    scratch[ent_off + 30] = (unsigned char)(len >> 16);
    scratch[ent_off + 31] = (unsigned char)(len >> 24);
    if (fs->wr(ent_lba, scratch) != 0) return -1;
    return 0;
}
