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

int fat32_mount(fat32_t *fs)
{
    unsigned char mbr[SD_BLOCK_SIZE];
    if (sd_read_block(0, mbr) != 0)             return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)   return -1;

    /* First partition table entry starts at MBR offset 0x1BE.
     *   +0x04 = partition type byte (0x0B / 0x0C for FAT32)
     *   +0x08 = LBA of first sector (LE u32) */
    unsigned char ptype = mbr[0x1BE + 0x04];
    if (ptype != 0x0B && ptype != 0x0C)         return -1;

    fs->part_lba = le32(&mbr[0x1BE + 0x08]);

    /* Read the partition's boot sector to get the BPB. */
    if (sd_read_block(fs->part_lba, scratch) != 0) return -1;
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

/* Read the FAT entry for `cluster`.  Returns the next-cluster value;
 * >= 0x0FFFFFF8 means end of chain.  Returns 0x0FFFFFFF on read
 * error so the caller terminates the walk. */
static unsigned int fat32_next_cluster(fat32_t *fs, unsigned int cluster)
{
    unsigned long fat_byte = (unsigned long)cluster * 4UL;
    unsigned long sec = fs->fat_lba + (fat_byte / SD_BLOCK_SIZE);
    unsigned int  off = (unsigned int)(fat_byte % SD_BLOCK_SIZE);

    if (sd_read_block(sec, scratch) != 0) return 0x0FFFFFFFu;
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
            if (sd_read_block(base + s, dirsec) != 0) return -1;

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
