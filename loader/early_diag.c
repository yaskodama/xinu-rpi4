// loader/early_diag.c — paint colour blocks at candidate FB addrs.
//
// See include/early_diag.h for the why.  The list of candidates is
// hand-picked from the "lower 4 GiB" Pi 5 RAM window, which is
// guaranteed to be mapped on every Pi 5 variant (4/8/16 GiB SKUs all
// have RAM here).  We avoid MMIO ranges (anything inside the
// 0x107C000000 / 0x107D000000 peripheral blocks) so a wrong guess
// doesn't trigger a data-abort to a missing exception vector.
//
// Each block is 256x256 px of ARGB8888 colour, written to memory and
// then flushed out of the D-cache so the HDMI controller's DMA can
// see it.  If any of these addresses is the actual framebuffer base,
// the user sees a solid coloured square stamped over the firmware's
// rainbow test pattern — and the colour tells us exactly which
// candidate hit.

#include "early_diag.h"

#define BLOCK_W       256
#define BLOCK_H       256
#define PITCH_PX     1920   /* assume worst-case 1920-wide FB        */
#define CACHE_LINE     64   /* Cortex-A76 D-cache line size          */

static void clean_dcache_range(unsigned long start, unsigned long size)
{
    unsigned long end = start + size;
    /* Align start down to cache-line boundary. */
    start &= ~(CACHE_LINE - 1UL);
    for (unsigned long a = start; a < end; a += CACHE_LINE) {
        __asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void paint_block(unsigned long addr, unsigned int colour)
{
    volatile unsigned int *p = (volatile unsigned int *)addr;
    for (int y = 0; y < BLOCK_H; y++) {
        for (int x = 0; x < BLOCK_W; x++) {
            p[y * PITCH_PX + x] = colour;
        }
    }
    /* Flush so any cached writes hit RAM where the HDMI DMA reads
     * from.  Cleaning by VA is cheap relative to a full ::sw flush
     * and avoids the "clean by set/way" instructions that may not
     * cover all the way to the point of coherency on Pi 5. */
    clean_dcache_range(addr, BLOCK_H * PITCH_PX * 4);
}

void early_paint_diagnostic(void)
{
#ifdef SKIP_MBOX
    /* QEMU virt has only 256 MiB RAM (0x40000000 + 0x10000000), so
     * the higher candidates would data-abort with no exception
     * vector installed.  We piggy-back on the same SKIP_MBOX define
     * the QEMU CFLAGS already sets — if there's no VC mailbox,
     * there's no HDMI either, so the diagnostic is moot anyway. */
    return;
#endif

    /* (address, ARGB colour, mnemonic) tuples.  If you see any
     * of these solid colours on screen, the corresponding address
     * is the firmware's framebuffer (or at least overlaps it). */
    static const struct {
        unsigned long addr;
        unsigned int  colour;
        /* tag is for the source comment; not printed. */
    } candidates[] = {
        { 0x40000000UL, 0xFFFF0000U },  /* RED   — 1 GiB mark        */
        { 0x60000000UL, 0xFF00FF00U },  /* GREEN — 1.5 GiB mark      */
        { 0x80000000UL, 0xFF0000FFU },  /* BLUE  — 2 GiB mark        */
        { 0xA0000000UL, 0xFFFFFF00U },  /* YELLOW — 2.5 GiB mark     */
        { 0xC0000000UL, 0xFFFF00FFU },  /* MAGENTA — 3 GiB mark      */
        { 0xE0000000UL, 0xFF00FFFFU },  /* CYAN  — 3.5 GiB mark      */
    };

    int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
    for (int i = 0; i < n; i++) {
        paint_block(candidates[i].addr, candidates[i].colour);
    }
}
