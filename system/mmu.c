// system/mmu.c — AArch64 virtual memory bring-up.
//
// Stage 1: enable the MMU + D/I caches with an identity map (RAM =
//          Normal cacheable, MMIO = Device-nGnRnE).  Identity means
//          every existing physical pointer keeps working; we just gain
//          caching (a big JIT speedup) and per-region attributes.
//
// Stage 2: W^X memory protection.  The 1 GiB RAM block is refined to
//          2 MiB blocks (L2), and the 2 MiB block holding the kernel
//          image is refined again to 4 KiB pages (L3) so each section
//          gets the right permissions:
//             .text   -> read-only, executable
//             .rodata -> read-only, no-execute
//             .data/.bss -> read-write, no-execute
//             heap    -> read-write, executable  (the JIT runs here; a
//                        documented W^X exception until a dedicated RX
//                        JIT pool exists)
//          So kernel code can't be overwritten and kernel data can't be
//          executed, while everything stays identity-mapped.
//
// Three statically-allocated 4 KiB tables (one each of L1/L2/L3) are
// enough: for every target RAM lives in a single 1 GiB block and the
// kernel image in a single 2 MiB block.

#include "uart.h"
#include "mmu.h"
#include "kmalloc.h"

extern char _start[];
extern char _etext[];
extern char _data[];
extern char _end[];

#define ONE_GB        (1UL << 30)
#define TWO_MB        (1UL << 21)
#define PAGE          (1UL << 12)
#define NENT          512                /* entries per 4 KiB table       */

/* descriptor low bits */
#define D_BLOCK       0x1UL              /* L1/L2 block                   */
#define D_TABLE       0x3UL              /* table pointer / L3 page       */
#define D_PAGE        0x3UL              /* L3 page                       */
#define D_AF          (1UL << 10)
#define D_SH_INNER    (3UL << 8)
#define D_SH_NONE     (0UL << 8)
#define D_AP_RO       (2UL << 6)         /* AP[2:1]=10: read-only EL1     */
#define D_AP_RW       (0UL << 6)
#define D_PXN         (1UL << 53)
#define D_UXN         (1UL << 54)
#define ATTRIDX(n)    ((unsigned long)(n) << 2)

#define ATTR_DEVICE   0
#define ATTR_NORMAL   1

static unsigned long __attribute__((aligned(4096))) l1_table[NENT];
static unsigned long __attribute__((aligned(4096))) l2_table[NENT];
/* 4 KiB pages for the whole kernel image, not just its first 2 MiB.
 *
 * There used to be ONE L3 table here, covering the 2 MiB block that holds
 * _start.  The image outgrew that long ago: with _data at 0x26a000 and _end
 * at ~0x4234000, everything past 0x200000 — the tail of .rodata and the
 * entire .data/.bss, which is where the DMA rings, proctab and smp_stack
 * live — fell back to the L2 2 MiB blocks and was mapped RW+X.  W^X was
 * silently off for ~65 MB of the address space.
 *
 * KIMG_2MB must cover [kern_2mb, heap_start).  Sized with headroom; the
 * runtime check below reports it if the image ever outgrows the array
 * instead of quietly degrading the way the single-table version did. */
#define KIMG_2MB      48                 /* 96 MiB of 4 KiB-page coverage  */
static unsigned long __attribute__((aligned(4096))) l3_table[KIMG_2MB][NENT];
unsigned long g_wx_uncovered;            /* bytes left as RW+X 2 MiB blocks */
static int g_mmu_on;

int mmu_enabled(void) { return g_mmu_on; }
void vm_demand_init(void);     /* demand-paged window setup (defined below) */

/* Normal-memory attribute word for a leaf (block or page), given
 * read-only? and execute-never? */
static unsigned long normal_attr(int ro, int xn)
{
    unsigned long a = ATTRIDX(ATTR_NORMAL) | D_SH_INNER | D_AF | D_UXN;
    a |= ro ? D_AP_RO : D_AP_RW;
    if (xn) a |= D_PXN;
    return a;
}

#ifdef DCACHE_ON
/* Invalidate the entire D-cache by set/way to the Point of Coherency, AArch64.
 * MUST run before enabling SCTLR.C: the cache holds garbage out of reset, and
 * enabling it without invalidating tells the core that garbage is valid data
 * (this bricked a Pi 3 once).  Standard CLIDR/CCSIDR walk (ARM ARM). */
static void dcache_invalidate_all(void)
{
    unsigned long clidr;
    __asm__ volatile ("mrs %0, clidr_el1" : "=r"(clidr));
    unsigned long loc = (clidr >> 24) & 7;
    for (unsigned long level = 0; level < loc; level++) {
        unsigned long type = (clidr >> (level * 3)) & 7;
        if (type < 2) continue;                       /* no D-side at this level */
        __asm__ volatile ("msr csselr_el1, %0\n isb\n" :: "r"(level << 1));
        unsigned long ccsidr;
        __asm__ volatile ("mrs %0, ccsidr_el1" : "=r"(ccsidr));
        unsigned int linesh = (unsigned int)(ccsidr & 7) + 4;      /* log2(line bytes) */
        unsigned int ways   = (unsigned int)((ccsidr >> 3)  & 0x3FF);
        unsigned int sets   = (unsigned int)((ccsidr >> 13) & 0x7FFF);
        unsigned int wayshift = (unsigned int)__builtin_clz(ways); /* ways at top */
        for (int w = (int)ways; w >= 0; w--)
            for (int s = (int)sets; s >= 0; s--) {
                unsigned long val = (level << 1)
                    | ((unsigned long)s << linesh)
                    | ((unsigned long)(unsigned int)w << wayshift);
                __asm__ volatile ("dc isw, %0" :: "r"(val));
            }
    }
    __asm__ volatile ("msr csselr_el1, xzr\n dsb sy\n isb\n" ::: "memory");
}

/* Clean (write back) the entire D-cache by set/way to the Point of Coherency,
 * AArch64.  Same CLIDR/CCSIDR walk as the invalidate above, but issues
 * `dc cisw` (clean + invalidate) so every dirty line is flushed to RAM.  Used
 * by the D-cache experiment to make CPU-written framebuffer pixels visible to
 * the GPU/HDMI scan-out (the GPU does not snoop the CPU cache): with C=1 the
 * bench text sits in the D-cache until this write-back reaches RAM.  Exposed
 * (non-static) so smpbench_serial_run() can flush the screen before halting. */
void dcache_clean_all(void)
{
    unsigned long clidr;
    __asm__ volatile ("mrs %0, clidr_el1" : "=r"(clidr));
    unsigned long loc = (clidr >> 24) & 7;
    for (unsigned long level = 0; level < loc; level++) {
        unsigned long type = (clidr >> (level * 3)) & 7;
        if (type < 2) continue;                       /* no D-side at this level */
        __asm__ volatile ("msr csselr_el1, %0\n isb\n" :: "r"(level << 1));
        unsigned long ccsidr;
        __asm__ volatile ("mrs %0, ccsidr_el1" : "=r"(ccsidr));
        unsigned int linesh = (unsigned int)(ccsidr & 7) + 4;      /* log2(line bytes) */
        unsigned int ways   = (unsigned int)((ccsidr >> 3)  & 0x3FF);
        unsigned int sets   = (unsigned int)((ccsidr >> 13) & 0x7FFF);
        unsigned int wayshift = (unsigned int)__builtin_clz(ways); /* ways at top */
        for (int w = (int)ways; w >= 0; w--)
            for (int s = (int)sets; s >= 0; s--) {
                unsigned long val = (level << 1)
                    | ((unsigned long)s << linesh)
                    | ((unsigned long)(unsigned int)w << wayshift);
                __asm__ volatile ("dc cisw, %0" :: "r"(val));
            }
    }
    __asm__ volatile ("msr csselr_el1, xzr\n dsb sy\n isb\n" ::: "memory");
}

/* Clean (write back) a VA range to the Point of Coherency, 64-byte lines
 * (Cortex-A72 L1/L2 cache line = 64 B).  Used before handing a RAM buffer to a
 * non-snooping agent (the VideoCore GPU via the property mailbox): with the
 * D-cache ON the CPU's writes sit in cache, so the GPU would read stale RAM. */
void dcache_clean_range(void *va, unsigned long size)
{
    unsigned long a   = (unsigned long)va & ~63UL;
    unsigned long end = (unsigned long)va + size;
    for (; a < end; a += 64)
        __asm__ volatile ("dc cvac, %0" :: "r"(a));
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Invalidate a VA range so the next read misses and refetches from RAM.  Used
 * after the GPU has written its mailbox response: the CPU holds a stale cached
 * copy and must drop it to see the firmware's reply.  Pure invalidate (not
 * clean+invalidate) — a write-back here would clobber the GPU's response with
 * the CPU's stale copy.  Safe against neighbours sharing the boundary lines:
 * mbox_call() cleans the same range first (writing any neighbour bytes to RAM),
 * and nothing writes those bytes between the clean and this invalidate. */
void dcache_inval_range(void *va, unsigned long size)
{
    unsigned long a   = (unsigned long)va & ~63UL;
    unsigned long end = (unsigned long)va + size;
    for (; a < end; a += 64)
        __asm__ volatile ("dc ivac, %0" :: "r"(a));
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Enable the Cortex-A72 SMP/coherency bit.  Unlike the A76 (rpi5), whose DSU
 * keeps shareable WB memory coherent with no software enable, the A72 needs
 * CPUECTLR_EL1.SMPEN (bit 6) set before its data cache participates in inner-
 * shareable coherency.  Without it, C=1 makes the lock-free worker-pool
 * mailbox non-coherent between cores (the classic x1.0 no-speedup symptom).
 * CPUECTLR_EL1 is S3_1_C15_C2_1 on the A72. */
static void __attribute__((unused)) a72_smp_enable(void)
{
    unsigned long ectlr;
    __asm__ volatile ("mrs %0, S3_1_C15_C2_1" : "=r"(ectlr));
    ectlr |= (1UL << 6);                    /* SMPEN */
    __asm__ volatile ("msr S3_1_C15_C2_1, %0\n isb\n" :: "r"(ectlr) : "memory");
}
#endif /* DCACHE_ON */

void mmu_init(void)
{
    /* Enable EL1 FP/SIMD access (CPACR_EL1.FPEN=0b11).  The kernel is built
     * -mgeneral-regs-only so it never touches FP itself, but the value_t
     * runtime in cc/cc.c (built with FP) needs floating point for AIPL
     * float values; without this an FP instruction traps. */
    __asm__ volatile ("msr cpacr_el1, %0\n isb\n" :: "r"(3UL << 20) : "memory");

    unsigned long ram_base  = ((unsigned long)_start) & ~(ONE_GB - 1);
    unsigned long ram_end   = HEAP_END;
    unsigned long kern_2mb  = ((unsigned long)_start) & ~(TWO_MB - 1);
    unsigned long etext     = (unsigned long)_etext;
    unsigned long data      = (unsigned long)_data;
    unsigned long heap_strt = (((unsigned long)_end) + PAGE - 1) & ~(PAGE - 1);

    /* ---- L3: every 2 MiB block of the kernel image, as 4 KiB pages ----
     * The heap keeps RW+X on purpose: cc/cc.c JITs into heap memory and then
     * branches to it, so making the heap NX would break the JIT.  Everything
     * below heap_start is now covered at page granularity. */
    int kimg_blocks = (int)((heap_strt - kern_2mb + TWO_MB - 1) / TWO_MB);
    if (kimg_blocks > KIMG_2MB) {
        g_wx_uncovered = (heap_strt - kern_2mb) - (unsigned long)KIMG_2MB * TWO_MB;
        kimg_blocks = KIMG_2MB;
    }
    for (int t = 0; t < kimg_blocks; t++) {
        for (int p = 0; p < NENT; p++) {
            unsigned long pa = kern_2mb + (unsigned long)t * TWO_MB
                                        + (unsigned long)p * PAGE;
            unsigned long attr;
            if      (pa <  (unsigned long)_start) attr = normal_attr(0, 1); /* below kernel: RW NX */
            else if (pa <  etext)                 attr = normal_attr(1, 0); /* .text:   RO  X      */
            /* .rodata は当面 RW のまま（元どおり）。
             * ★ 2026-09-05: ここを RO にして実機で試した。結果:
             *   - 0xDE038 の 1 バイトの化けは **RO にしても起きた** ―― つまりその
             *     書き込みはこの写像を通っていない（別の写像か、MMU の効かない文脈。
             *     副コア（smp.c）や、L3 で覆い切れていない領域が疑わしい。
             *     mmu_init は kimg_blocks を KIMG_2MB で打ち切り、余りを
             *     g_wx_uncovered に記録している ―― そこを確認すること）
             *   - さらに、RO にすると通常動作でも板が落ちた ＝ **正規の書き込みが
             *     .rodata に入っている**。RO 化は、その洗い出しとセットでないと成立しない
             * どちらも次の一手の手掛かりなので、消さずに残す。 */
            else if (pa <  data)                  attr = normal_attr(0, 1); /* .rodata: RW  NX     */
            else if (pa <  heap_strt)             attr = normal_attr(0, 1); /* data/bss: RW NX     */
            else                                  attr = normal_attr(0, 0); /* heap:    RW  X      */
            l3_table[t][p] = pa | attr | D_PAGE;
        }
    }

    /* ---- L2: the 1 GiB RAM block, 2 MiB blocks ---- */
    for (int r = 0; r < NENT; r++) {
        unsigned long pa = ram_base + (unsigned long)r * TWO_MB;
        if (pa >= kern_2mb && pa < kern_2mb + (unsigned long)kimg_blocks * TWO_MB) {
            int t = (int)((pa - kern_2mb) / TWO_MB);
            l2_table[r] = (unsigned long)l3_table[t] | D_TABLE;       /* -> L3 */
        } else if (pa >= ram_base && pa < ram_end) {
            l2_table[r] = pa | normal_attr(0, 0) | D_BLOCK;           /* heap/RAM: RW X */
        } else {
            l2_table[r] = 0;                                         /* invalid (beyond RAM) */
        }
    }

    /* ---- L1: 1 GiB blocks; the RAM block points at the L2 table ---- */
    for (int i = 0; i < NENT; i++) {
        unsigned long addr = (unsigned long)i << 30;
        if (addr == ram_base) {
            l1_table[i] = (unsigned long)l2_table | D_TABLE;          /* -> L2 */
        } else {
            l1_table[i] = addr | ATTRIDX(ATTR_DEVICE) | D_SH_NONE | D_AF
                               | D_PXN | D_UXN | D_BLOCK;             /* MMIO: Device, XN */
        }
    }

    unsigned long mair = (0x00UL << (8 * ATTR_DEVICE)) | (0xFFUL << (8 * ATTR_NORMAL));
    /* Non-cacheable table walks (IRGN0=ORGN0=00) to match D-cache being
     * left OFF below — see the SCTLR comment for why. */
    unsigned long tcr =
          (25UL << 0)     /* T0SZ = 39-bit VA              */
        | (0UL  << 8)     /* IRGN0 = Non-cacheable         */
        | (0UL  << 10)    /* ORGN0 = Non-cacheable         */
        | (3UL  << 12)    /* SH0   = inner shareable       */
        | (0UL  << 14)    /* TG0   = 4 KiB                 */
        | (1UL  << 23)    /* EPD1  = disable TTBR1 walks   */
        | (2UL  << 32);   /* IPS   = 40-bit PA             */

    __asm__ volatile (
        "dsb sy\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1,  %1\n"
        "msr ttbr0_el1,%2\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((unsigned long)l1_table) : "memory");

    unsigned long sctlr;
    __asm__ volatile (
        "ic iallu\n"
        "dsb sy\n"
        "isb\n"
        "mrs %0, sctlr_el1\n" : "=r"(sctlr));
    sctlr |= (1UL << 0);    /* M — MMU enable                              */
    sctlr |= (1UL << 12);   /* I — I-cache enable (speeds instruction fetch,
                             *     incl. JIT'd code; no DMA hazard)        */
    /* D-cache (C, bit 2) is intentionally left OFF.  The GENET RX/TX rings
     * and the VideoCore mailbox/framebuffer are DMA'd by hardware straight
     * to RAM and the drivers assume uncached access; enabling the D-cache
     * would make them incoherent.  With C=0 every data access goes to RAM
     * directly — identical coherency to the old MMU-off world — so the MMU
     * (translation + W^X) is safe to run on real hardware.  The page table
     * already marks RAM Normal-cacheable, so a future DMA-coherent design
     * can flip C on without re-tabling. */
#ifdef DCACHE_ON
    /* ★ Experiment: the D-cache (C=1) is enabled LATER, by mmu_enable_dcache()
     * called from main() AFTER video_init().  Enabling C here (before the FB
     * mailbox handshake) made the VideoCore read a stale, cached request and
     * HDMI never came up.  Bringing HDMI up with C=0 first, then flipping C=1,
     * keeps the mailbox coherent and lets the bench results reach the screen. */
#endif
    __asm__ volatile (
        "msr sctlr_el1, %0\n"
        "isb\n"
        :: "r"(sctlr) : "memory");

    g_mmu_on = 1;

    vm_demand_init();      /* arm the demand-paged virtual window (no backing yet) */
}

#ifdef DCACHE_ON
/* Enable the D-cache (SCTLR.C=1) on core 0, deferred until AFTER video_init()
 * so the framebuffer mailbox handshake ran coherently with C=0.  Order is
 * critical: turn on A72 SMP coherency (SMPEN) and invalidate the D-cache by
 * set/way BEFORE setting C=1 (the cache holds reset garbage; enabling C without
 * invalidating tells the core the garbage is valid — this bricked a Pi 3 once).
 * The secondary cores enable their own C=1 the same way in mmu_enable_secondary,
 * brought up by smp_init() which main() calls right after this. */
void mmu_enable_dcache(void)
{
    extern void screen_puts(const char *);
    extern void video_flush(void);
    unsigned long sctlr;

    /* NB: a72_smp_enable() (writing CPUECTLR_EL1.SMPEN) is deliberately NOT
     * called here.  This kernel runs at EL1 (boot.S drops EL2->EL1), and on the
     * Cortex-A72 the CPUECTLR_EL1 (S3_1_C15_C2_1) IMP-DEF register is not
     * accessible from EL1 unless ACTLR_EL2/EL3.CPUECTLR was set by higher ELs —
     * which the Pi 4 firmware does not do.  The MRS therefore traps to EL2
     * (VBAR_EL2 = 0) and the core dead-jumps to address 0.  Without SMPEN the
     * D-cache still works for single-core; cross-core coherency of the
     * lock-free job mailbox may break (bench will report agree=NO / no speedup),
     * which is itself the observation.  Markers print each step so a fault is
     * localised on screen (serial is dead, net is incoherent with C=1). */

    screen_puts("  [dcache] invalidate D$ by set/way ...\n"); video_flush();
    dcache_invalidate_all();

    screen_puts("  [dcache] set SCTLR.C=1 ...\n"); video_flush();
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 2);    /* C — D-cache enable */
    __asm__ volatile ("msr sctlr_el1, %0\n isb\n" :: "r"(sctlr) : "memory");

    /* C=1 now: this screen write is cached, so flush it to RAM for the GPU. */
    screen_puts("  [dcache] C=1 live.\n"); video_flush();
}
#endif /* DCACHE_ON */

/* Bring a secondary core's MMU up to the SAME configuration core 0 uses, so
 * all four cores execute identically (fair benchmarking): identity map via the
 * page tables core 0 already built (l1_table), MMU + I-cache ON, D-cache OFF
 * (SCTLR.C=0 — see the note in mmu_init).  Called from smp_secondary_entry()
 * (system/smp.c) on cores 1-3.  Does NOT rebuild the tables. */
void mmu_enable_secondary(void)
{
    unsigned long mair = (0x00UL << (8 * ATTR_DEVICE)) | (0xFFUL << (8 * ATTR_NORMAL));
    unsigned long tcr =
          (25UL << 0)     /* T0SZ = 39-bit VA      */
        | (0UL  << 8)     /* IRGN0 = Non-cacheable */
        | (0UL  << 10)    /* ORGN0 = Non-cacheable */
        | (3UL  << 12)    /* SH0   = inner shareable */
        | (0UL  << 14)    /* TG0   = 4 KiB         */
        | (1UL  << 23)    /* EPD1  = disable TTBR1 */
        | (2UL  << 32);   /* IPS   = 40-bit PA     */
    __asm__ volatile (
        "dsb sy\n tlbi vmalle1\n dsb sy\n isb\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1,  %1\n"
        "msr ttbr0_el1,%2\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((unsigned long)l1_table) : "memory");
    unsigned long sctlr;
    __asm__ volatile ("ic iallu\n dsb sy\n isb\n mrs %0, sctlr_el1\n" : "=r"(sctlr));
    sctlr |= (1UL << 0);    /* M — MMU enable    */
    sctlr |= (1UL << 12);   /* I — I-cache enable (D-cache C stays OFF) */
#ifdef DCACHE_ON
    /* Each worker core set/way-invalidates its own D-cache (it also holds reset
     * garbage) before enabling C=1.  SMPEN is NOT set here: like core 0
     * (mmu_enable_dcache), writing CPUECTLR_EL1 from EL1 traps on the A72 with
     * this firmware.  So the workers run C=1 WITHOUT hardware cross-core
     * coherency — the bench's agree= column will reveal whether the lock-free
     * mailbox survives (expected: coherency breaks, mirroring the A76/Pi 5). */
    dcache_invalidate_all();
    sctlr |= (1UL << 2);    /* C — D-cache enable (DCACHE_ON) */
#endif
    __asm__ volatile ("msr sctlr_el1, %0\n isb\n" :: "r"(sctlr) : "memory");
}

/* ====================================================================
 *  Stage 3: map an arbitrary virtual address to a chosen physical page
 *  and demonstrate that translation works.
 * ==================================================================== */

/* A virtual window high above the identity region (32 GiB).  It is in an
 * otherwise-unused 1 GiB L1 slot for every target (Pi 4 / QEMU use only
 * low memory), so repurposing it as a page table is safe. */
#define VMAP_VA   0x800000000UL          /* L1 index 32 */

static unsigned long __attribute__((aligned(4096))) l2_win[NENT];
static unsigned long __attribute__((aligned(4096))) l3_win[NENT];

/* Map one 4 KiB page: VMAP_VA -> `pa`, Normal cacheable RW NX.  Returns
 * the virtual address.  (Single fixed window — enough for the demo.) */
static void *mmu_map_window(unsigned long pa)
{
    int l1i = (int)(VMAP_VA >> 30);
    l3_win[0]   = (pa & ~(PAGE - 1)) | normal_attr(0, 1) | D_PAGE;
    l2_win[0]   = (unsigned long)l3_win | D_TABLE;
    l1_table[l1i] = (unsigned long)l2_win | D_TABLE;

    __asm__ volatile ("dsb sy\n tlbi vmalle1\n dsb sy\n isb\n" ::: "memory");
    return (void *)(VMAP_VA | (pa & (PAGE - 1)));
}

/* `vmtest` shell command: prove VA->PA translation. */
static void put_hex(unsigned long v)
{
    char b[2 + 16 + 1]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned long n = (v >> ((15 - i) * 4)) & 0xF;
        b[2 + i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
    }
    b[18] = 0; uart_puts(b);
}

int cmd_vmtest(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!g_mmu_on) { uart_puts("vmtest: MMU is off\n"); return -1; }

    volatile unsigned long *phys = (volatile unsigned long *)kmalloc(PAGE);
    if (!phys) { uart_puts("vmtest: out of memory\n"); return -1; }

    volatile unsigned long *va = (volatile unsigned long *)mmu_map_window((unsigned long)phys);

    uart_puts("vmtest: phys page = "); put_hex((unsigned long)phys); uart_puts("\n");
    uart_puts("        virtual   = "); put_hex((unsigned long)va);   uart_puts("  (32 GiB window)\n");

    *va = 0xC0FFEE01UL;                  /* write through the virtual mapping */
    uart_puts("        wrote 0xc0ffee01 via VA; read via PA = "); put_hex(*phys);
    uart_puts(*phys == 0xC0FFEE01UL ? "  OK\n" : "  MISMATCH\n");

    *phys = 0x1234ABCDUL;                /* write through the physical alias */
    uart_puts("        wrote 0x1234abcd via PA; read via VA = "); put_hex(*va);
    uart_puts(*va == 0x1234ABCDUL ? "  OK\n" : "  MISMATCH\n");

    uart_puts("        => VA and PA differ but alias the same page: translation works.\n");
    kfree((void *)phys);
    return 0;
}

/* ====================================================================
 *  Demand-paged virtual memory.  A virtual window with NO physical backing
 *  until first touch: accessing an unmapped page raises a translation abort;
 *  sync_dispatch_c() calls vm_fault(), which grabs a physical frame from a
 *  pool, installs the L3 page-table entry, flushes the TLB, and returns so the
 *  faulting instruction re-executes against the now-valid mapping.
 * ==================================================================== */
#define VMD_BASE       0x80000000UL          /* L1 index 2 (unused device slot) */
#define VMD_L1_IDX     2
#define VMD_2MB        2                      /* 2 x 2 MiB = 4 MiB virtual window */
#define VMD_END        (VMD_BASE + (unsigned long)VMD_2MB * TWO_MB)
#define VMD_POOL_PAGES 512                    /* physical frames available on demand */

static unsigned long __attribute__((aligned(4096))) l2_vmd[NENT];
static unsigned long __attribute__((aligned(4096))) l3_vmd[VMD_2MB][NENT];
static unsigned char __attribute__((aligned(4096))) vmd_pool[VMD_POOL_PAGES][PAGE];
static int vmd_pool_next;
volatile unsigned long g_vm_faults, g_vm_mapped, g_vm_oom;

/* Build the page tables for the demand window with every leaf INVALID, then
 * splice the L2 table into L1[VMD_L1_IDX].  After this, any access in
 * [VMD_BASE, VMD_END) translation-faults until vm_fault() maps it. */
void vm_demand_init(void)
{
    for (int i = 0; i < NENT; i++) l2_vmd[i] = 0;
    for (int t = 0; t < VMD_2MB; t++) {
        for (int p = 0; p < NENT; p++) l3_vmd[t][p] = 0;        /* invalid */
        l2_vmd[t] = (unsigned long)&l3_vmd[t][0] | D_TABLE;
    }
    l1_table[VMD_L1_IDX] = (unsigned long)l2_vmd | D_TABLE;
    __asm__ volatile ("dsb sy\n tlbi vmalle1\n dsb sy\n isb\n" ::: "memory");
}

int vm_demand_region(unsigned long *base, unsigned long *size)
{ if (base) *base = VMD_BASE; if (size) *size = VMD_END - VMD_BASE; return VMD_POOL_PAGES; }

unsigned long vm_fault_count(void)  { return g_vm_faults; }
unsigned long vm_mapped_count(void) { return g_vm_mapped; }
unsigned long vm_oom_count(void)    { return g_vm_oom; }

/* Page-fault handler (called from sync_dispatch_c on a translation abort).
 * Returns 1 if `va` is in the demand window and is now mapped (retry the
 * instruction), 0 otherwise (not ours -> normal fault path). */
int vm_fault(unsigned long va)
{
    if (va < VMD_BASE || va >= VMD_END) return 0;
    unsigned long off = va - VMD_BASE;
    int l2i = (int)(off >> 21);
    int l3i = (int)((off >> 12) & 0x1FF);
    if (l2i < 0 || l2i >= VMD_2MB) return 0;
    unsigned long *pte = &l3_vmd[l2i][l3i];
    if (*pte & 1UL) return 1;                          /* already present (spurious) */
    if (vmd_pool_next >= VMD_POOL_PAGES) { g_vm_oom++; return 0; }   /* out of frames */
    unsigned long pa = (unsigned long)&vmd_pool[vmd_pool_next++][0];
    for (int i = 0; i < (int)(PAGE / 8); i++) ((volatile unsigned long *)pa)[i] = 0;  /* zero-fill */
    *pte = (pa & ~(PAGE - 1)) | normal_attr(0, 1) | D_PAGE;          /* Normal RW NX */
    __asm__ volatile ("dsb ish" ::: "memory");
    __asm__ volatile ("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory");
    __asm__ volatile ("dsb ish\n isb\n" ::: "memory");
    g_vm_faults++; g_vm_mapped++;
    return 1;
}

/* `vmdemand` shell command: touch N pages across the demand window (each first
 * touch faults and gets a fresh zero page), then read them back. */
int cmd_vmdemand(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!g_mmu_on) { uart_puts("vmdemand: MMU is off\n"); return -1; }
    unsigned long f0 = g_vm_faults;
    volatile unsigned char *p = (volatile unsigned char *)VMD_BASE;
    int npages = 64;
    uart_puts("vmdemand: window VA "); put_hex(VMD_BASE);
    uart_puts(" .. "); put_hex(VMD_END); uart_puts(" (no backing until touched)\n");
    for (int i = 0; i < npages; i++) p[(unsigned long)i * PAGE] = (unsigned char)(i ^ 0xA5);
    int ok = 1;
    for (int i = 0; i < npages; i++)
        if (p[(unsigned long)i * PAGE] != (unsigned char)(i ^ 0xA5)) ok = 0;
    uart_puts("  touched 64 pages; faults this run = "); put_hex(g_vm_faults - f0);
    uart_puts(", total mapped = "); put_hex(g_vm_mapped);
    uart_puts(ok ? ", readback OK\n" : ", readback MISMATCH\n");
    uart_puts("  => each page had no RAM until its first access faulted it in.\n");
    return 0;
}
