// kernel/proc.h — process / scheduler interface.
//
// Mirrors classic Embedded Xinu:
//   - proctab[] holds each process's state, priority, name, stack
//     base/size, and the saved kernel SP that ctxsw() will reload.
//   - NULLPROC (pid 0) is the original boot context (kernel_main /
//     shell).  It is never placed on the ready list; instead it
//     becomes the resume target when nothing else is ready.
//   - resched() is the dispatcher; user code normally calls
//     proc_yield() (resched) or proc_exit() (resched + free slot).
//
// Scheduling is preemptive fixed-priority (proc_set_preempt) over a
// generic-timer one-shot, with round-robin among equal priorities:
//   - Ready processes live in per-priority FIFOs indexed by a 64-bit
//     bitmap, so dispatch is O(1) (a CLZ) rather than a list walk.
//     This matters here: NPROC is 2048, the actor workload keeps ~1300
//     ready, and the D-cache is off, so every link traversal is a DRAM
//     round trip.
//   - The running process is requeued BEFORE the next one is picked, so
//     its own priority participates in the comparison.  Without that a
//     tick hands the CPU to a lower-priority task (which is what made
//     "priority preemption" behave as plain round-robin).
//   - Sleepers live on a list sorted by deadline, so the timer ISR looks
//     at the head instead of scanning all NPROC entries twice.

#ifndef XINU_RPI4_PROC_H
#define XINU_RPI4_PROC_H

#define NPROC          2048  /* 2026-05-31: 256 -> 512 -> 1024 -> 2048.
                              * N-Queens N=8 needed 1467 spawn attempts and
                              * peak ~1300 concurrent alive (with recycling) —
                              * 1024 hit 444 spawn_fails and hung; 2048 leaves
                              * headroom for N=8 + the AP_QLEN reduction below.
                              * Memory budget: 8 KB stack/proc x 2048 = 16 MB.
                              * g_act[] in actorproc.c at AP_QLEN=64 takes
                              * 2047 x (64 x 48 + 16) ≈ 6.3 MB.  Total well
                              * under HEAP_END=0x40000000.  IMPORTANT: aipl2c's
                              * g_obj[N] in c_translator.ml must match NPROC
                              * exactly — any slot id >= bound silently corrupts
                              * BSS via g_spawn() write.  Current pairing: both
                              * NPROC and g_obj at 2048. */
#define NULLPROC       0
#define PROC_NAME_LEN  16
#define PROC_DEFAULT_STK   4096UL

enum proc_state {
    PR_FREE = 0,   /* slot unused                                */
    PR_READY,      /* on ready list, waiting for CPU             */
    PR_CURR,       /* currpid points here                        */
    PR_WAIT,       /* blocked (e.g. on an empty mailbox)         */
    PR_SLEEP,      /* timed sleep: readied by the timer tick      */
    PR_TERM        /* exited, awaiting reaper                    */
};

typedef void (*proc_entry_t)(void);

/* Scheduling priorities are clamped to [0, PROC_PRIO_MAX] because the ready
 * bitmap is one 64-bit word.  0 is NULLPROC's; 1 is the default; the RT
 * harness uses 50. */
#define PROC_PRIO_MAX       63
#define PROC_DEFAULT_PRIO 1

/* Written at stkbase (the LOWEST address of a process stack, i.e. the far
 * end from where SP starts) so an overflow destroys it.  Checked at every
 * context-switch point.  Cheap: one load and compare per switch. */
#define PROC_STK_CANARY 0xC0DEFEEDDEADBEEFUL

struct procent {
    enum proc_state state;
    int             prio;
    void           *stkbase;
    unsigned long   stklen;
    void           *sp;             /* saved kernel SP            */
    void           *arg;            /* opaque per-process argument */
    char            name[PROC_NAME_LEN];
    /* Queue links.  While PR_READY these thread the per-priority ready
     * FIFO (doubly linked so proc_kill can unlink in O(1)); while
     * PR_SLEEP `next` threads the deadline-sorted sleep list.  A process
     * is never on both. */
    struct procent *next;
    struct procent *prev;
    int             qprio;          /* priority this was queued AT      */
    unsigned long   wake_at_us;     /* PR_SLEEP: CNTPCT-us release time */
};

extern struct procent proctab[NPROC];
extern int            currpid;

void proc_init(void);
int  proc_create(proc_entry_t entry, unsigned long stksize, const char *name);
/* Like proc_create but stashes `arg` in proctab[pid].arg so the new
 * process can recover it (e.g. which actor it is) on first run. */
int  proc_create_arg(proc_entry_t entry, unsigned long stksize, const char *name, void *arg);
void proc_ready(int pid);
void proc_resched(void);
void proc_yield(void);
void proc_exit(void);
/* Real-time additions (P1): priority-ordered dispatch + timed sleep.
 * proc_setprio() sets a process's scheduling priority (higher = runs first,
 * now that ready dispatch is priority-ordered).  proc_sleep_us() blocks the
 * caller until `us` microseconds have passed; the timer tick (proc_timer_tick)
 * readies due sleepers and requests a preemptive switch, so a high-priority
 * periodic task wakes and preempts lower-priority compute.  These only affect
 * processes that call them — resident actors keep running cooperatively. */
void proc_setprio(int pid, int prio);
void proc_sleep_us(unsigned long us);
void proc_timer_tick(void);     /* called from the timer IRQ handler */
unsigned long proc_next_delay_us(void);  /* us to next RT deadline (tickless one-shot) */
/* Preemptive scheduling (timer-driven round-robin).  proc_set_preempt(1)
 * enables it; proc_resched_request() is called from the timer ISR; and
 * proc_preempt() (after the IRQ is EOI'd) performs the switch. */
void proc_set_preempt(int on);
void proc_resched_request(void);
void proc_preempt(void);
/* Suppress timer preemption while the cooperative actor pump runs (actors are
 * ready-list vheap users that would otherwise race the scheduler under
 * preemption).  Counted; bracket actor execution (cc.c).  Plain non-actor
 * compute keeps full preemption. */
void proc_actor_pump_enter(void);
void proc_actor_pump_leave(void);
void proc_actor_pump_force_clear(void);   /* 例外からの復帰専用 */
/* Live runtime accessors (read by the HDMI runtime monitor). */
int           proc_preempt_on(void);
unsigned long proc_ctxsw_count(void);
/* Stack-overflow detection: how many times a process was found with its
 * canary destroyed, and who it was last time.  A non-zero count means some
 * process ran off the bottom of its stack into the neighbouring heap block
 * — the damage is already done, but this makes it visible instead of
 * silent.  Remember the IRQ frame alone is 768 bytes. */
unsigned long proc_stk_bad_count(void);
int           proc_stk_bad_pid(void);
const char   *proc_stk_bad_name(void);
/* Ready-queue depth (diagnostic): total ready processes, and the highest
 * priority currently runnable (-1 if none). */
int proc_ready_count(void);
int proc_ready_top_prio(void);
/* Block the current process (removes it from CURR; resched won't re-ready
 * it) until proc_ready() puts it back.  Used by mailbox receive. */
void proc_block(void);

/* AIPL vheap mutex (spin-yield): serializes the cc/vheap runtime across
 * actors / app worker / shell so preemption can't interleave two vheap ops.
 * Acquire around cc execution bursts; aipl_unlock_all/aipl_relock bracket a
 * voluntary block (ap_select) so the cooperative pump still runs. */
void aipl_lock(void);
void aipl_unlock(void);
int  aipl_unlock_all(void);   /* release fully; returns depth to restore */
void aipl_relock(int saved);  /* reacquire to a saved depth after a block */
void aipl_force_release(void);/* drop the lock unconditionally (fault/reset path) */
/* Lock state for the HDMI runtime monitor: which pid holds the vheap (-1 if
 * free) and the recursion depth.  Read from NULLPROC so it survives a wedge. */
int  proc_aipl_owner(void);
int  proc_aipl_depth(void);
/* Free a blocked/ready process's slot + stack (used to reap actor
 * processes after a one-shot run).  Must not be the current process. */
void proc_kill(int pid);

/* AArch64 callee-saved (x19-x30) save / restore.  Implemented in
 * ctxsw.S.  *old_sp receives the SP value to resume `old` later;
 * new_sp is loaded immediately. */
extern void ctxsw(void **old_sp, void *new_sp);

#endif /* XINU_RPI4_PROC_H */
