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

/* ---- D-cache experiment (DCACHE_ON): keep the lock-free mailbox and the
 * secondary-core bring-up data coherent WITHOUT SMPEN.  On the A72 SMPEN
 * (CPUECTLR_EL1) is not writable from EL1 with the Pi 4 firmware, so instead of
 * hardware coherency we clean writes to RAM and invalidate before reads at each
 * cross-core sync point.  These are `dc cvac`/`dc ivac` by VA (legal at EL1, no
 * trap).  Compiled out (no-ops) in the normal D-cache-OFF build. */
#ifdef DCACHE_ON
extern void dcache_clean_range(void *, unsigned long);
extern void dcache_inval_range(void *, unsigned long);
#define MB_CLEAN(p)  dcache_clean_range((void *)(p), sizeof *(p))
#define MB_INVAL(p)  dcache_inval_range((void *)(p), sizeof *(p))
#else
#define MB_CLEAN(p)  ((void)0)
#define MB_INVAL(p)  ((void)0)
#endif

int smp_core_id(void)
{
    unsigned long m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    return (int)(m & 3);
}

/* The worker idle loop: wait (low-power) for a new job, run it, signal done. */
static void smp_worker_loop(int core)
{
    MB_INVAL(&smp_job_seq[core]);
    int last = smp_job_seq[core];
    for (;;) {
        MB_INVAL(&smp_job_seq[core]);
        while (smp_job_seq[core] == last) { __asm__ volatile("wfe"); MB_INVAL(&smp_job_seq[core]); }
        last = smp_job_seq[core];
        MB_INVAL(&smp_job_fn[core]); MB_INVAL(&smp_job_lo[core]); MB_INVAL(&smp_job_hi[core]);
        smp_range_fn fn = smp_job_fn[core];
        long r = fn ? fn(smp_job_lo[core], smp_job_hi[core], core) : 0;
        smp_job_res[core] = r;
        MB_CLEAN(&smp_job_res[core]);
        dsb();
        smp_job_done[core] = last;       /* publish completion after the result */
        MB_CLEAN(&smp_job_done[core]);
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
    MB_CLEAN(&smp_online[core]);             /* flush so core 0 sees us (no SMPEN) */
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
        /* With the D-cache ON these writes sit in core 0's cache; the secondary
         * reads its stack top in boot.S with the cache OFF and would get stale
         * RAM (null stack -> dies on first push).  Clean them to RAM first. */
        MB_CLEAN(&smp_job_seq[c]); MB_CLEAN(&smp_job_done[c]); MB_CLEAN(&smp_stacktop[c]);
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
        /* The parked secondary / firmware spin-table reads these with the cache
         * OFF, so flush them to RAM before the SEV or the core never releases. */
        MB_CLEAN(&smp_release[c]); MB_CLEAN(spin_mbox[c]);
    }
    dsb_sev();

    /* Wait (bounded) for each to announce itself online. */
    for (int c = 1; c < SMP_NCORES; c++) {
        unsigned long spins = 0;
        MB_INVAL(&smp_online[c]);
        while (!smp_online[c] && ++spins < SMP_BRINGUP_WAIT) { __asm__ volatile("nop"); MB_INVAL(&smp_online[c]); }
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
        /* Publish the job params to RAM before bumping seq, so the worker (which
         * invalidates before reading) sees this job's args, not a stale set. */
        MB_CLEAN(&smp_job_fn[c]); MB_CLEAN(&smp_job_lo[c]); MB_CLEAN(&smp_job_hi[c]);
        dsb();
        smp_job_seq[c]++;        /* arm the job, then wake the worker */
        MB_CLEAN(&smp_job_seq[c]);
        dsb_sev();
    }

    /* Core 0 runs chunk 0 inline while the workers run theirs. */
    total += fn(0, (ncores == 1) ? n : chunk, 0);

    /* Collect the workers, taking over any that did not finish in time. */
    for (int c = 1; c < ncores; c++) {
        if (!smp_online[c]) continue;          /* already done inline above */
        unsigned long spins = 0;
        MB_INVAL(&smp_job_done[c]);
        while (smp_job_done[c] != smp_job_seq[c]) {
            if (++spins >= SMP_WAIT_LIMIT) {   /* worker stuck — do it here */
                long lo = (long)c * chunk;
                long hi = (c == ncores - 1) ? n : lo + chunk;
                smp_job_res[c] = fn(lo, hi, 0);
                break;
            }
            __asm__ volatile("nop");
            MB_INVAL(&smp_job_done[c]);
        }
        MB_INVAL(&smp_job_res[c]);
        total += smp_job_res[c];
    }
    return total;
}
