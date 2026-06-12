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
static unsigned long __attribute__((aligned(4096))) l3_table[NENT];
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

    /* ---- L3: the 2 MiB block holding the kernel image, 4 KiB pages ---- */
    for (int p = 0; p < NENT; p++) {
        unsigned long pa = kern_2mb + (unsigned long)p * PAGE;
        unsigned long attr;
        if      (pa <  (unsigned long)_start) attr = normal_attr(0, 1); /* below kernel: RW NX */
        else if (pa <  etext)                 attr = normal_attr(1, 0); /* .text:   RO  X      */
        else if (pa <  data)                  attr = normal_attr(1, 1); /* .rodata: RO  NX     */
        else if (pa <  heap_strt)             attr = normal_attr(0, 1); /* data/bss: RW NX     */
        else                                  attr = normal_attr(0, 0); /* heap:    RW  X      */
        l3_table[p] = pa | attr | D_PAGE;
    }

    /* ---- L2: the 1 GiB RAM block, 2 MiB blocks ---- */
    for (int r = 0; r < NENT; r++) {
        unsigned long pa = ram_base + (unsigned long)r * TWO_MB;
        if (pa == kern_2mb) {
            l2_table[r] = (unsigned long)l3_table | D_TABLE;          /* -> L3 */
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
    __asm__ volatile (
        "msr sctlr_el1, %0\n"
        "isb\n"
        :: "r"(sctlr) : "memory");

    g_mmu_on = 1;

    vm_demand_init();      /* arm the demand-paged virtual window (no backing yet) */
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
