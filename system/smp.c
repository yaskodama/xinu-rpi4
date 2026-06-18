// system/smp.c — worker-pool SMP bring-up + lock-free parallel dispatch.
// See include/smp.h and docs/SMP_REPORT_JA.md for the architecture rationale.

#include "smp.h"

/* ---- boot.S handoff (these live in boot.S's .data so they read as 0 from
 * image load, before core 0 clears .bss — a secondary spinning in the wfe
 * poll must never see a stale non-zero release address). ---- */
extern volatile unsigned long smp_release[SMP_NCORES];   /* entry addr per core */
extern volatile unsigned long smp_stacktop[SMP_NCORES];  /* initial SP per core */
extern void _smp_start(void);                            /* boot.S trampoline   */

/* Each secondary core's idle stack (no heap dependency at bring-up time). */
#define SMP_STACK_BYTES 16384
static unsigned char smp_stack[SMP_NCORES][SMP_STACK_BYTES] __attribute__((aligned(16)));

/* Online flags + the per-core job mailbox.  All cross-core, all volatile;
 * coherent without locks because the D-cache is off (every access hits RAM). */
static volatile int          smp_online[SMP_NCORES];
static volatile smp_range_fn smp_job_fn[SMP_NCORES];
static volatile long         smp_job_lo[SMP_NCORES];
static volatile long         smp_job_hi[SMP_NCORES];
static volatile long         smp_job_res[SMP_NCORES];
static volatile int          smp_job_seq[SMP_NCORES];    /* bumped to post a job */
static volatile int          smp_job_done[SMP_NCORES];   /* == seq when finished */

/* Bound on how long core 0 waits for a worker before taking the chunk over
 * itself.  ~1e9 spin iterations ≈ a few seconds — far longer than any real
 * chunk, so it only trips on a genuinely dead/never-started core. */
#define SMP_WAIT_LIMIT 2000000000UL

/* Bring-up wait: a released core announces itself in microseconds, so cap the
 * per-core online wait short (~tens of ms) — if a core does not respond in that
 * window it is treated as offline and boot proceeds (no multi-second stall). */
#define SMP_BRINGUP_WAIT 100000000UL

static inline void dsb_sev(void) { __asm__ volatile("dsb sy\n\tsev" ::: "memory"); }
static inline void dsb(void)     { __asm__ volatile("dsb sy" ::: "memory"); }

int smp_core_id(void)
{
    unsigned long m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    return (int)(m & 3);
}

/* The worker idle loop: wait (low-power) for a new job, run it, signal done. */
static void smp_worker_loop(int core)
{
    int last = smp_job_seq[core];
    for (;;) {
        while (smp_job_seq[core] == last) __asm__ volatile("wfe");
        last = smp_job_seq[core];
        smp_range_fn fn = smp_job_fn[core];
        long r = fn ? fn(smp_job_lo[core], smp_job_hi[core], core) : 0;
        smp_job_res[core] = r;
        dsb();
        smp_job_done[core] = last;       /* publish completion after the result */
        dsb_sev();                       /* wake core 0 out of its wait spin     */
    }
}

/* C entry for a freshly-started secondary core (called from boot.S at EL1 with
 * its stack already set).  Match core 0's MMU/cache config for fair timing,
 * install the shared exception vectors, announce online, then idle. */
void smp_secondary_entry(int core)
{
    extern void exception_init(void);       /* VBAR_EL1 -> shared vector table */
    extern void mmu_enable_secondary(void); /* MMU on, I-cache on, D-cache off */
    if (core < 0 || core >= SMP_NCORES) { for (;;) __asm__ volatile("wfe"); }
    exception_init();
    mmu_enable_secondary();
    smp_online[core] = 1;
    dsb_sev();                              /* tell core 0 we are up */
    smp_worker_loop(core);                  /* never returns */
}

void smp_init(void)
{
    smp_online[0] = 1;                       /* core 0 is obviously up */
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_job_seq[c]  = 0;
        smp_job_done[c] = 0;
        smp_stacktop[c] = (unsigned long)(smp_stack[c] + SMP_STACK_BYTES);
    }
    dsb();

    /* Release each secondary via BOTH mechanisms, then SEV:
     *   (a) the firmware spin-table mailbox (default Pi 4 armstub holds cores
     *       1-3 spinning on phys 0xe0/0xe8/0xf0 until a function addr is
     *       written there), and
     *   (b) smp_release[] which boot.S's own wfe-park path polls — in case a
     *       given firmware instead drops all four cores into kernel _start.
     * Whichever path a core is actually on, it converges on _smp_start. */
    static volatile unsigned long *const spin_mbox[SMP_NCORES] =
        { 0, (volatile unsigned long *)0xe0UL,
             (volatile unsigned long *)0xe8UL,
             (volatile unsigned long *)0xf0UL };
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_release[c] = (unsigned long)&_smp_start;
        *spin_mbox[c]  = (unsigned long)&_smp_start;
    }
    dsb_sev();

    /* Wait (bounded) for each to announce itself online. */
    for (int c = 1; c < SMP_NCORES; c++) {
        unsigned long spins = 0;
        while (!smp_online[c] && ++spins < SMP_BRINGUP_WAIT) __asm__ volatile("nop");
    }
}

int smp_cores_online(void)
{
    int n = 0;
    for (int c = 0; c < SMP_NCORES; c++) if (smp_online[c]) n++;
    return n;
}

long smp_parallel_sum(smp_range_fn fn, long n, int ncores)
{
    if (ncores < 1) ncores = 1;
    if (ncores > SMP_NCORES) ncores = SMP_NCORES;
    if (n < 0) n = 0;

    long chunk = n / ncores;
    long total = 0;

    /* Post chunks 1..ncores-1 to the worker cores (skip offline ones — those
     * chunks are computed by core 0 below). */
    for (int c = 1; c < ncores; c++) {
        long lo = (long)c * chunk;
        long hi = (c == ncores - 1) ? n : lo + chunk;
        if (!smp_online[c]) { total += fn(lo, hi, 0); continue; }
        smp_job_fn[c] = fn;
        smp_job_lo[c] = lo;
        smp_job_hi[c] = hi;
        dsb();
        smp_job_seq[c]++;        /* arm the job, then wake the worker */
        dsb_sev();
    }

    /* Core 0 runs chunk 0 inline while the workers run theirs. */
    total += fn(0, (ncores == 1) ? n : chunk, 0);

    /* Collect the workers, taking over any that did not finish in time. */
    for (int c = 1; c < ncores; c++) {
        if (!smp_online[c]) continue;          /* already done inline above */
        unsigned long spins = 0;
        while (smp_job_done[c] != smp_job_seq[c]) {
            if (++spins >= SMP_WAIT_LIMIT) {   /* worker stuck — do it here */
                long lo = (long)c * chunk;
                long hi = (c == ncores - 1) ? n : lo + chunk;
                smp_job_res[c] = fn(lo, hi, 0);
                break;
            }
            __asm__ volatile("nop");
        }
        total += smp_job_res[c];
    }
    return total;
}
