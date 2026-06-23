// system/kexec.c — runtime OS switcher, independent of the blender display (avm.c).
//
// `kexec status` (shell, or GET /kexec) lists every *.IMG on the USB /sd (S:) and
// microSD (M:); `kexec NAME.IMG` streams it off the card a few clusters per frame
// (kexec_tick, driven by the WM loop, with a load bar) into the chainload stage
// 0x4000000 and warm-boots it.  Self-contained so OS1/OS2 can drop avm.o entirely.

#include "fat32.h"
#include "video.h"
#include "wm.h"

extern int  usbmsd_ready(void);
extern int  usbmsd_read_block(unsigned long, void *);
extern int  usbmsd_write_block(unsigned long, const void *);
extern int  sd_read_block(unsigned long, void *);
extern void kernel_chainload(unsigned long stage, unsigned long len);

static int kx_streq(const char *a, const char *b)
{ while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b; }

static int kx_mount(char vol, fat32_t *fs)
{
    if (vol == 'S') { if (!usbmsd_ready()) return -1;
                      return fat32_mount_dev(fs, usbmsd_read_block, usbmsd_write_block); }
    return fat32_mount(fs);                          /* 'M' = microSD (EMMC) */
}

/* ===== scan SD/microSD for *.IMG candidates ============================== */
#define KX_MAX 16
static struct { char name[16]; char vol; unsigned int size; } kx_list[KX_MAX];
static int kx_n;
static void kx_visit(const char *name, int is_dir, unsigned long size,
                     unsigned int fc, int depth, void *ctx)
{
    (void)fc; (void)depth;
    if (is_dir) return;
    int l = 0; while (name[l]) l++;
    if (l < 5) return;
    const char *e = name + l - 4;                    /* ".IMG" (any case) */
    if (!(e[0]=='.' && (e[1]=='I'||e[1]=='i') && (e[2]=='M'||e[2]=='m') && (e[3]=='G'||e[3]=='g'))) return;
    if (kx_n < KX_MAX) {
        int i = 0; for (; name[i] && i < 15; i++) kx_list[kx_n].name[i] = name[i];
        kx_list[kx_n].name[i] = 0;
        kx_list[kx_n].vol = *(char *)ctx; kx_list[kx_n].size = (unsigned int)size; kx_n++;
    }
}
int kexec_scan(void)
{
    kx_n = 0;
    static unsigned char probe[512];
    fat32_t fs; char v;
    if (usbmsd_ready() && usbmsd_read_block(0, probe) == 0) {
        v = 'S'; if (kx_mount('S', &fs) == 0) fat32_walk_dir(&fs, fs.root_cluster, 0, kx_visit, &v);
    }
    if (sd_read_block(0, probe) == 0) {
        v = 'M'; if (kx_mount('M', &fs) == 0) fat32_walk_dir(&fs, fs.root_cluster, 0, kx_visit, &v);
    }
    return kx_n;
}
int         kexec_count(void) { return kx_n; }
const char *kexec_name(int i) { return (i >= 0 && i < kx_n) ? kx_list[i].name : ""; }
char        kexec_vol(int i)  { return (i >= 0 && i < kx_n) ? kx_list[i].vol  : '?'; }
int         kexec_size(int i) { return (i >= 0 && i < kx_n) ? (int)kx_list[i].size : 0; }

/* ===== resumable warm-boot read (streamed by kexec_tick from the WM loop) == */
/* Stream into a DEDICATED buffer, not directly into the chainload stage
 * (0x4000000): kexec_tick keeps running between frames, and writing the image
 * over 0x4000000 incrementally would trample whatever heap/stack lives there
 * (that froze the load bar mid-way + wedged the box).  kernel_chainload() can
 * relocate from any source, so we hand it kx_buf — copied to 0x80000 + jumped
 * in one shot, with nothing using kx_buf afterwards. */
#define KX_BUF_MAX (2u * 1024u * 1024u)            /* largest kernel image (~1.9 MB) */
static unsigned char kx_buf[KX_BUF_MAX];
static int          kx_pend;
static fat32_t      kx_fs;
static unsigned int kx_cur, kx_got, kx_want, kx_sec, kx_fail;

/* Queue a warm-boot of NAME.IMG.  Returns 0 = queued (kexec_tick streams it +
 * jumps), <0 = setup error.  Non-blocking: a multi-MB microSD read MUST be paced
 * per frame or it starves the box for minutes. */
int kexec_boot(const char *name)
{
    kexec_scan();
    int idx = -1;
    /* Prefer the USB /sd copy (S:): its block reads are far more reliable than the
     * microSD (EMMC) on this Pi4, so a multi-MB image streams without stalling. */
    for (int i = 0; i < kx_n; i++) if (kx_list[i].vol == 'S' && kx_streq(kx_list[i].name, name)) { idx = i; break; }
    if (idx < 0) for (int i = 0; i < kx_n; i++) if (kx_streq(kx_list[i].name, name)) { idx = i; break; }
    if (idx < 0) return -1;
    if (kx_mount(kx_list[idx].vol, &kx_fs) != 0) return -2;
    unsigned int fc = 0, sz = 0;
    if (fat32_open(&kx_fs, name, &fc, &sz) != 0 || fc < 2) return -3;
    if (sz == 0 || sz > KX_BUF_MAX) return -6;        /* image too big for the buffer */
    kx_cur = fc; kx_got = 0; kx_sec = 0; kx_fail = 0; kx_want = sz; kx_pend = 1;
    return 0;
}

/* Called once per WM frame.  Streams a few clusters into 0x4000000; on completion
 * jumps via kernel_chainload.  Draws a load bar while in flight. */
void kexec_tick(void)
{
    if (!kx_pend) return;
    unsigned int spc = kx_fs.sectors_per_cluster ? kx_fs.sectors_per_cluster : 1;
    int budget = 32; unsigned char sec[512];
    while (kx_pend && budget-- > 0) {
        if (kx_got >= kx_want) {                      /* whole image in kx_buf -> jump */
            kx_pend = 0;
            kernel_chainload((unsigned long)kx_buf, (unsigned long)kx_got);   /* never returns */
            return;
        }
        if (!(kx_cur >= 2 && kx_cur < 0x0FFFFFF8u)) { kx_pend = 0; return; }  /* chain end: abort */
        unsigned long base = fat32_cluster_lba(&kx_fs, kx_cur);
        int rok = 0;
        for (int t = 0; t < 8 && !rok; t++) if (kx_fs.rd(base + kx_sec, sec) == 0) rok = 1;
        if (!rok) { if (++kx_fail > 1200) kx_pend = 0; break; }  /* yield, retry same sector next frame */
        kx_fail = 0;
        unsigned int chunk = kx_want - kx_got; if (chunk > 512) chunk = 512;
        if (kx_got + chunk <= KX_BUF_MAX)
            for (unsigned int i = 0; i < chunk; i++) kx_buf[kx_got + i] = sec[i];
        kx_got += chunk;
        if (++kx_sec >= spc) { kx_sec = 0; kx_cur = fat32_next_cluster_of(&kx_fs, kx_cur); }
    }
    if (kx_pend) {                                    /* progress bar */
        int sw = (int)video_screen_width(), sh = (int)video_screen_height();
        int bw = 420, bh = 30, bx = (sw - bw) / 2, by = sh - 90;
        int pct = kx_want ? (int)((long)kx_got * 100 / kx_want) : 0; if (pct > 100) pct = 100;
        fill_rect(bx - 4, by - 20, bw + 8, bh + 28, 0xFF0A1420U);
        draw_rect(bx - 4, by - 20, bw + 8, bh + 28, 0xFF40A0FFU);
        draw_string_at(bx, by - 16, "kexec: streaming kernel image...", 0xFFE8F0F8U, 0xFF0A1420U);
        draw_rect(bx, by, bw, bh, 0xFF6080A0U);
        fill_rect(bx + 1, by + 1, (bw - 2) * pct / 100, bh - 2, 0xFF30D040U);
    }
}
