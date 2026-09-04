// kernel/proc.c — cooperative scheduler over `proctab[]`.
//
// Pattern follows Embedded Xinu's system/resched.c / system/create.c:
//   - Single global ready list (FIFO in this Round-1 cut; Xinu uses
//     a priority queue — we can drop that in later without changing
//     callers).
//   - resched() saves the current SP into proctab[currpid].sp,
//     loads the next ready process's SP, and ctxsw()'s.
//   - create() pre-loads a fake "ctxsw save frame" on the new
//     stack so the first ctxsw INTO the new process pops it and
//     returns directly into the entry function.

#include "proc.h"
#include "memory.h"
#include "critical.h"

struct procent proctab[NPROC];
int            currpid;

/* Preemption (timer-driven round-robin).  Off by default: the cooperative
 * AIPL/actor/LLM runtime shares non-reentrant global state (the value_t heap,
 * the LLM buffers, GENET), so we only preempt when explicitly enabled around
 * isolated, self-contained processes.  The timer ISR sets g_resched_pending;
 * proc_preempt() (run after the IRQ is EOI'd) acts on it. */
static volatile int g_preempt_on;
static volatile int g_resched_pending;
static volatile unsigned long g_ctxsw;     /* context switches (live diagnostic) */
/* P1 preemption diagnostics (why doesn't a woken RT task preempt a CPU hog?) */
static volatile unsigned long g_dbg_pp_calls;   /* proc_preempt() entered        */
static volatile unsigned long g_dbg_pp_offgate;  /* returned: !on || !pending     */
static volatile unsigned long g_dbg_pp_actorgate;/* returned: actor pump active   */
static volatile unsigned long g_dbg_pp_fired;    /* reached proc_resched()        */
static volatile unsigned long g_dbg_tick_wakes;  /* sleepers readied by the tick  */
static volatile unsigned long g_dbg_hi_dispatch; /* ready_pop returned prio>=40    */
unsigned long proc_dbg_ppcalls(void)  { return g_dbg_pp_calls; }
unsigned long proc_dbg_ppoff(void)    { return g_dbg_pp_offgate; }
unsigned long proc_dbg_ppactor(void)  { return g_dbg_pp_actorgate; }
unsigned long proc_dbg_ppfired(void)  { return g_dbg_pp_fired; }
unsigned long proc_dbg_wakes(void)    { return g_dbg_tick_wakes; }
unsigned long proc_dbg_hidisp(void)   { return g_dbg_hi_dispatch; }
void proc_set_preempt(int on)      { g_preempt_on = on ? 1 : 0; }
void proc_resched_request(void)    { g_resched_pending = 1; }

/* Preemption gate for the cooperative actor pump.  The preemptive-networking
 * safety argument assumes the only ready-list vheap user is the app worker (so
 * a timer preempt lands only on the net process).  Resident AIPL actors break
 * that: they are vheap users sitting on the ready list, so a preempt can
 * interleave the app worker's ap_run / an actor handler with another actor and
 * race the actor scheduling (the MultiAgent wedge).  While the actor pump is
 * active we therefore suppress preemption — actors run cooperatively (the
 * proven-safe mode).  Plain compute with no actors (e.g. /llm) keeps full
 * preemption, so its network-latency win is unaffected.  Counter (not a flag)
 * so nested actor execution composes. */
static volatile int g_actor_pump;
void proc_actor_pump_enter(void) { g_actor_pump++; }
void proc_actor_pump_leave(void) { if (g_actor_pump > 0) g_actor_pump--; }
/* 例外からの復帰でだけ使う。アクタの協調実行中に落ちると、この旗が立ったままで
   proc_preempt() が何もしないので、recover_spin が wfi で回っても他のプロセスが
   走らない ―― 板ごと死んで /fault さえ読めなくなる。強制的に落とす。 */
void proc_actor_pump_force_clear(void) { g_actor_pump = 0; }

/* Live runtime accessors for the HDMI monitor (drawn by the wm in NULLPROC,
 * so they stay visible even when the app worker / HTTP path wedges). */
int           proc_preempt_on(void)  { return g_preempt_on; }
unsigned long proc_ctxsw_count(void) { return g_ctxsw; }

/* AIPL vheap mutex state (the lock/unlock impls are further down).  Declared
 * here so proc_kill() can release the lock if it reaps the process that holds
 * it — otherwise a dead owner makes every later vheap user spin-yield forever
 * (the app worker wedges; HTTP dies while ICMP survives). */
static volatile int g_aipl_owner = -1;
static volatile int g_aipl_depth;

/* Lock watchdog (diagnostic): if a spinner can't acquire after this many
 * yields the holder is wedged (never releasing), so we record the culprit and
 * force-acquire — the app worker then never sticks, keeping HTTP/diagnostics
 * alive.  Each yield normally lets the holder run to a release point, so the
 * count only climbs when the holder is blocked/dead; this won't false-trigger
 * on a legitimately long (e.g. LLM) hold. */
#define AIPL_SPIN_LIMIT 300000
volatile unsigned long g_lock_timeouts;
volatile int           g_lock_stuck_owner;        /* pid that wouldn't release */
volatile int           g_lock_stuck_owner_state;  /* its proctab state         */
volatile int           g_lock_stuck_by;           /* pid that gave up waiting   */

/* Lock state accessors for the HDMI runtime monitor (read from NULLPROC). */
int proc_aipl_owner(void) { return g_aipl_owner; }
int proc_aipl_depth(void) { return g_aipl_depth; }

/* ---------------- Ready queue: per-priority FIFO + bitmap ----------------
 *
 * rq_head/rq_tail[p] is the FIFO of ready processes at priority p, and bit p
 * of rq_bitmap is set exactly when that FIFO is non-empty.  Dispatch is then
 * "highest set bit" = one CLZ, and enqueue/dequeue/remove are all O(1).
 *
 * The previous implementation was a single list walked linearly on every
 * dispatch.  With NPROC=2048, ~1300 ready actors under the N-Queens workload,
 * and SCTLR.C=0 (D-cache deliberately off, see include/smp.h), that walk was
 * ~1300 DRAM round trips per context switch.
 *
 * Queue links live in procent.next/prev, and qprio records the priority the
 * process was *inserted at* so a proc_setprio() between enqueue and removal
 * cannot make us unlink from the wrong FIFO. */
static struct procent *rq_head[PROC_PRIO_MAX + 1];
static struct procent *rq_tail[PROC_PRIO_MAX + 1];
static unsigned long long rq_bitmap;
static int rq_count;                     /* diagnostic only */

static int prio_clamp(int prio)
{
    if (prio < 0)        return 0;
    if (prio > PROC_PRIO_MAX) return PROC_PRIO_MAX;
    return prio;
}

static void copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < PROC_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void ready_push(struct procent *p)
{
    int q = prio_clamp(p->prio);
    p->qprio = q;
    p->next  = 0;
    p->prev  = rq_tail[q];
    if (rq_tail[q]) rq_tail[q]->next = p;
    else            rq_head[q]       = p;
    rq_tail[q] = p;
    rq_bitmap |= 1ULL << q;
    rq_count++;
}

/* Unlink a node from its FIFO.  Caller guarantees it is queued. */
static void rq_unlink(struct procent *p)
{
    int q = p->qprio;
    if (p->prev) p->prev->next = p->next;
    else         rq_head[q]    = p->next;
    if (p->next) p->next->prev = p->prev;
    else         rq_tail[q]    = p->prev;
    if (rq_head[q] == 0) rq_bitmap &= ~(1ULL << q);
    p->next = p->prev = 0;
    rq_count--;
}

/* Highest-priority ready process, FIFO among equals.  0 if none. */
static struct procent *ready_pop(void)
{
    if (rq_bitmap == 0) return 0;
    int q = 63 - __builtin_clzll(rq_bitmap);
    struct procent *best = rq_head[q];
    rq_unlink(best);
    if (q >= 40) g_dbg_hi_dispatch++;      /* RT proc actually dispatched */
    return best;
}

/* Priority of the best ready process, or -1 when nothing is ready. */
static int ready_best_prio(void)
{
    if (rq_bitmap == 0) return -1;
    return 63 - __builtin_clzll(rq_bitmap);
}

/* Unlink `target` from the ready list if present.  A killed process MUST
 * leave the ready list: otherwise ready_pop() could later hand back a
 * PR_FREE slot, and — worse — once that slot is reused (e.g. by a new
 * actor) the stale link makes ready_push wire the node to itself, so
 * ready_pop returns the *running* process and proc_block ctxsw()'s into a
 * frame the process has already overwritten (return address = garbage). */
static void ready_remove(struct procent *target)
{
    if (target->state != PR_READY) return;   /* not queued */
    rq_unlink(target);
}

int proc_ready_count(void)    { return rq_count; }
int proc_ready_top_prio(void) { return ready_best_prio(); }

/* ---------------- Sleep queue: sorted by deadline ------------------------
 *
 * Sleepers hang off sleep_head in ascending wake_at_us order, threaded
 * through procent.next (a process is never on the ready list and the sleep
 * list at the same time).  The timer ISR then reads the head instead of
 * scanning all NPROC entries — twice, which is what proc_next_delay_us() and
 * proc_timer_tick() used to do on every one-shot fire, at up to 5 kHz. */
static struct procent *sleep_head;

static void sleep_insert(struct procent *p)
{
    struct procent *prev = 0, *curr = sleep_head;
    while (curr && curr->wake_at_us <= p->wake_at_us) {
        prev = curr;
        curr = curr->next;
    }
    p->next = curr;
    if (prev) prev->next = p;
    else      sleep_head = p;
}

static void sleep_remove(struct procent *p)
{
    struct procent *prev = 0, *curr = sleep_head;
    while (curr) {
        if (curr == p) {
            if (prev) prev->next = curr->next;
            else      sleep_head = curr->next;
            curr->next = 0;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/* ---------------- Stack canary ------------------------------------------ */
static volatile unsigned long g_stk_bad;
static volatile int           g_stk_bad_pid = -1;
static char                   g_stk_bad_name[PROC_NAME_LEN];

unsigned long proc_stk_bad_count(void) { return g_stk_bad; }
int           proc_stk_bad_pid(void)   { return g_stk_bad_pid; }
const char   *proc_stk_bad_name(void)  { return g_stk_bad_name; }

/* Verify a process has not run off the bottom of its stack.  NULLPROC has no
 * allocated stack (it inherits boot.S's), so it is skipped — see the note in
 * proc_init.  Called at every switch point; one uncached load. */
static void stk_check(struct procent *p)
{
    if (p->stkbase == 0) return;
    if (*(volatile unsigned long *)p->stkbase == PROC_STK_CANARY) return;
    g_stk_bad++;
    g_stk_bad_pid = (int)(p - proctab);
    copy_name(g_stk_bad_name, p->name);
    /* Re-arm so one overflowing process doesn't count on every switch;
     * the counter records that it happened at least once. */
    *(volatile unsigned long *)p->stkbase = PROC_STK_CANARY;
}

void proc_init(void)
{
    int i;
    for (i = 0; i < NPROC; i++) {
        proctab[i].state = PR_FREE;
        proctab[i].next  = 0;
        proctab[i].prev  = 0;
        proctab[i].qprio = 0;
    }
    for (i = 0; i <= PROC_PRIO_MAX; i++) rq_head[i] = rq_tail[i] = 0;
    rq_bitmap  = 0;
    rq_count   = 0;
    sleep_head = 0;

    /* NULLPROC = the live boot/shell context. We don't allocate a
     * stack for it (it inherits boot.S's stack at _start) and we
     * leave .sp = 0 until the first ctxsw OUT writes the real SP.
     * That also means it gets no canary: its stack starts at _start
     * (0x80000) and grows down into the firmware spin table with no
     * known bound.  Giving NULLPROC a real bounded stack is a separate
     * change (see docs/OS_ASSESSMENT_JA.md). */
    struct procent *p = &proctab[NULLPROC];
    p->state   = PR_CURR;
    p->prio    = 0;
    p->stkbase = 0;
    p->stklen  = 0;
    p->sp      = 0;
    copy_name(p->name, "null/shell");

    currpid    = NULLPROC;
}

/* Claim a free slot.  Masked, and the slot is marked non-free before we let
 * go, so two concurrent creators cannot be handed the same pid.  PR_TERM is
 * used as the "claimed but not yet runnable" marker: alloc_slot skips it,
 * proc_ready() refuses it, and it is never on a queue. */
static int alloc_slot(void)
{
    unsigned long d = irq_save();
    for (int i = 1; i < NPROC; i++) {
        if (proctab[i].state == PR_FREE) {
            proctab[i].state = PR_TERM;
            irq_restore(d);
            return i;
        }
    }
    irq_restore(d);
    return -1;
}

int proc_create(proc_entry_t entry, unsigned long stksize, const char *name)
{
    return proc_create_arg(entry, stksize, name, 0);
}

int proc_create_arg(proc_entry_t entry, unsigned long stksize, const char *name, void *arg)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;

    /* Minimum stack: the IRQ entry frame alone is 768 bytes (31 GPRs + all
     * 32 q-registers, see exception_vectors.S), pushed onto whichever
     * process was interrupted.  A 1 KB stack could not survive one IRQ plus
     * a nested resched, so the floor is 2 KB and there is a canary at the
     * base to catch what still overflows. */
    if (stksize < 2048) stksize = 2048;
    stksize = ROUNDMB(stksize);

    /* If a previous occupant of this slot died via proc_exit (self-exit),
     * its stack memory was never freed (proc_exit cannot freemem its own
     * live stack).  Free it now, before allocating the new slot's stack —
     * otherwise long-lived spawn-and-suicide patterns leak ~stksize bytes
     * per cycle and exhaust the heap.  proctab[pid].stkbase==0 after
     * proc_kill() so this is a no-op for kill-reaped slots. */
    void         *old_stk = proctab[pid].stkbase;
    unsigned long old_len = proctab[pid].stklen;
    proctab[pid].stkbase  = 0;
    proctab[pid].stklen   = 0;
    if (old_stk) freemem(old_stk, old_len);

    void *stk = getmem(stksize);
    if (stk == 0) { proctab[pid].state = PR_FREE; return -1; }   /* release the claim */

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = PROC_DEFAULT_PRIO;
    p->stkbase = stk;
    p->stklen  = stksize;
    p->arg     = arg;
    copy_name(p->name, name);
    p->next    = 0;
    p->prev    = 0;

    /* Canary at the lowest address of the stack — the end SP grows toward. */
    *(volatile unsigned long *)stk = PROC_STK_CANARY;

    /* Lay out an initial saved-register frame at the top of the
     * stack, in the exact order ctxsw.S restores them:
     *   [sp +   0] x29 (FP)
     *   [sp +   8] x30 (LR)   <-- where `ret` jumps; we put `entry` here
     *   [sp +  16] d14, d15   <-- AAPCS64 callee-saved FP (low 64 bits)
     *   [sp +  32] d12, d13
     *   [sp +  48] d10, d11
     *   [sp +  64] d8,  d9
     *   [sp +  80] x27, x28
     *   [sp +  96] x25, x26
     *   [sp + 112] x23, x24
     *   [sp + 128] x21, x22
     *   [sp + 144] x19, x20
     * 20 quadwords = 160 bytes, keeping the 16-byte SP alignment.
     * Initial FP regs are zeroed — fresh process has no meaningful
     * FP state. */
    extern void proc_entry_trampoline(void);   /* ctxsw.S: enables IRQs then br x19 */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    unsigned long *sp     = sp_top - 20;
    sp[0]  = 0;                          /* x29 (FP)                                */
    sp[1]  = (unsigned long)proc_entry_trampoline;  /* x30 (LR): enable IRQs, br x19 */
    sp[2]  = 0; sp[3]  = 0;              /* d14, d15            */
    sp[4]  = 0; sp[5]  = 0;              /* d12, d13            */
    sp[6]  = 0; sp[7]  = 0;              /* d10, d11            */
    sp[8]  = 0; sp[9]  = 0;              /* d8,  d9             */
    sp[10] = 0; sp[11] = 0;              /* x27, x28            */
    sp[12] = 0; sp[13] = 0;              /* x25, x26            */
    sp[14] = 0; sp[15] = 0;              /* x23, x24            */
    sp[16] = 0; sp[17] = 0;              /* x21, x22            */
    sp[18] = (unsigned long)entry;       /* x19 -> trampoline br target (entry) */
    sp[19] = 0;                          /* x20                 */
    p->sp = (void *)sp;

    /* Masked: the timer ISR pushes woken sleepers onto the same queue from
     * proc_timer_tick(), and it runs even while preemption is disabled.  An
     * unprotected push here raced it — with the actor workload spawning
     * constantly at up to 5 kHz of timer IRQs, a torn insert dropped the new
     * process off the run queue and it never ran again.  (The old single
     * linked list had the same race; the doubly-linked FIFO + bitmap simply
     * has more invariants to tear.) */
    unsigned long d = irq_save();
    ready_push(p);
    irq_restore(d);
    return pid;
}

/* proc_create_arg と同じだが、スタックを呼び出し側が渡す（getmem を使わない）。
   アクタのように何度も作り直すものは、毎回 getmem/freemem すると空き領域を
   刻み続ける。静的な池から配れば churn が消える。
   ★ 渡したスタックは呼び出し側の持ち物。proc_kill が freemem しないよう
     stkbase は 0 のままにし、canary だけ置く。 */
int proc_create_static_arg(proc_entry_t entry, void *stk, unsigned long stksize,
                           const char *name, void *arg)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;

    /* Minimum stack: the IRQ entry frame alone is 768 bytes (31 GPRs + all
     * 32 q-registers, see exception_vectors.S), pushed onto whichever
     * process was interrupted.  A 1 KB stack could not survive one IRQ plus
     * a nested resched, so the floor is 2 KB and there is a canary at the
     * base to catch what still overflows. */
    if (stksize < 2048) stksize = 2048;
    stksize = ROUNDMB(stksize);

    /* If a previous occupant of this slot died via proc_exit (self-exit),
     * its stack memory was never freed (proc_exit cannot freemem its own
     * live stack).  Free it now, before allocating the new slot's stack —
     * otherwise long-lived spawn-and-suicide patterns leak ~stksize bytes
     * per cycle and exhaust the heap.  proctab[pid].stkbase==0 after
     * proc_kill() so this is a no-op for kill-reaped slots. */
    { void *old_stk = proctab[pid].stkbase; unsigned long old_len = proctab[pid].stklen;
      proctab[pid].stkbase = 0; proctab[pid].stklen = 0;
      if (old_stk) freemem(old_stk, old_len); }
    if (stk == 0) { proctab[pid].state = PR_FREE; return -1; }

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = PROC_DEFAULT_PRIO;
    p->stkbase = 0;             /* 池の持ち物。proc_kill に freemem させない */
    p->stklen  = stksize;
    p->arg     = arg;
    copy_name(p->name, name);
    p->next    = 0;
    p->prev    = 0;

    /* Canary at the lowest address of the stack — the end SP grows toward. */
    *(volatile unsigned long *)stk = PROC_STK_CANARY;

    /* Lay out an initial saved-register frame at the top of the
     * stack, in the exact order ctxsw.S restores them:
     *   [sp +   0] x29 (FP)
     *   [sp +   8] x30 (LR)   <-- where `ret` jumps; we put `entry` here
     *   [sp +  16] d14, d15   <-- AAPCS64 callee-saved FP (low 64 bits)
     *   [sp +  32] d12, d13
     *   [sp +  48] d10, d11
     *   [sp +  64] d8,  d9
     *   [sp +  80] x27, x28
     *   [sp +  96] x25, x26
     *   [sp + 112] x23, x24
     *   [sp + 128] x21, x22
     *   [sp + 144] x19, x20
     * 20 quadwords = 160 bytes, keeping the 16-byte SP alignment.
     * Initial FP regs are zeroed — fresh process has no meaningful
     * FP state. */
    extern void proc_entry_trampoline(void);   /* ctxsw.S: enables IRQs then br x19 */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    unsigned long *sp     = sp_top - 20;
    sp[0]  = 0;                          /* x29 (FP)                                */
    sp[1]  = (unsigned long)proc_entry_trampoline;  /* x30 (LR): enable IRQs, br x19 */
    sp[2]  = 0; sp[3]  = 0;              /* d14, d15            */
    sp[4]  = 0; sp[5]  = 0;              /* d12, d13            */
    sp[6]  = 0; sp[7]  = 0;              /* d10, d11            */
    sp[8]  = 0; sp[9]  = 0;              /* d8,  d9             */
    sp[10] = 0; sp[11] = 0;              /* x27, x28            */
    sp[12] = 0; sp[13] = 0;              /* x25, x26            */
    sp[14] = 0; sp[15] = 0;              /* x23, x24            */
    sp[16] = 0; sp[17] = 0;              /* x21, x22            */
    sp[18] = (unsigned long)entry;       /* x19 -> trampoline br target (entry) */
    sp[19] = 0;                          /* x20                 */
    p->sp = (void *)sp;

    /* Masked: the timer ISR pushes woken sleepers onto the same queue from
     * proc_timer_tick(), and it runs even while preemption is disabled.  An
     * unprotected push here raced it — with the actor workload spawning
     * constantly at up to 5 kHz of timer IRQs, a torn insert dropped the new
     * process off the run queue and it never ran again.  (The old single
     * linked list had the same race; the doubly-linked FIFO + bitmap simply
     * has more invariants to tear.) */
    unsigned long d = irq_save();
    ready_push(p);
    irq_restore(d);
    return pid;
}

void proc_ready(int pid)
{
    if (pid <= 0 || pid >= NPROC) return;
    unsigned long d = irq_save();
    struct procent *p = &proctab[pid];
    /* Idempotent: only enqueue a process that is actually blocked.  A double
     * ready (already PR_READY = on the list, or PR_CURR = running) would
     * ready_push the node a second time, wiring its `next` into a self-loop
     * (see ready_remove's note) — ready_pop then returns a running process and
     * proc_block ctxsw()s into an overwritten frame.  Under preemption a wake
     * (ap_post) can race a preempt that already parked the target on the ready
     * list; this guard stops the resulting list corruption, which otherwise
     * drops an actor from scheduling and livelocks ap_run (the MultiAgent wedge:
     * app=WORKING, hb frozen, lock own=-1, sw still climbing). */
    if (p->state == PR_READY || p->state == PR_CURR) { irq_restore(d); return; }
    /* A freed or exited slot must never be queued: ready_pop would hand back
     * a PR_FREE entry whose stack has already been returned to the heap, and
     * the next ctxsw would jump into reused memory. */
    if (p->state == PR_FREE || p->state == PR_TERM) { irq_restore(d); return; }
    /* Waking a timed sleeper early: take it off the deadline list first,
     * otherwise it sits on both lists and the two sets of links collide. */
    if (p->state == PR_SLEEP) sleep_remove(p);
    p->state = PR_READY;
    ready_push(p);
    irq_restore(d);
}

/* Pick the next process to run and ctxsw into it.  Returns once we resume on
 * the original stack.  A no-op when nothing better is ready, which is what
 * makes proc_yield() safe to call unconditionally.
 *
 * IMPORTANT — order of operations.  The running process is requeued BEFORE
 * the next one is picked, so its own priority takes part in the comparison.
 * The previous version popped first and pushed the runner back afterwards,
 * so the runner never competed: a timer tick would hand a prio-50 RT task's
 * CPU to a prio-5 hog.  That is why "priority preemption" measured as plain
 * round-robin.  Pushing first also gives round-robin among equals for free —
 * the runner goes to the tail of its own FIFO, so an equal-priority peer is
 * picked ahead of it, while a strictly-lower-priority peer is not. */
void proc_resched(void)
{
    unsigned long d = irq_save();

    struct procent *oldp = &proctab[currpid];
    int old_pid          = currpid;

    stk_check(oldp);

    /* NULLPROC never goes on the ready list (it is the fallback target), so
     * it does not participate — anything ready outranks it. */
    if (oldp->state == PR_CURR && old_pid != NULLPROC) {
        oldp->state = PR_READY;
        ready_push(oldp);
    }

    struct procent *newp = ready_pop();
    if (newp == 0) {
        /* Nothing ready at all — only reachable when we did not requeue,
         * i.e. NULLPROC or an already-blocked caller. */
        irq_restore(d);
        return;
    }

    if (newp == oldp) {
        /* We are still the best choice: stay put, no context switch.  This is
         * the common case for a high-priority task that yields while only
         * lower-priority work is pending. */
        oldp->state = PR_CURR;
        irq_restore(d);
        return;
    }

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);
    g_ctxsw++;

    ctxsw(&oldp->sp, newp->sp);
    /* Returns here when somebody ctxsw()'s back to us. */
    irq_restore(d);
}

void proc_yield(void)
{
    proc_resched();
}

/* ---- Real-time additions (P1): priority + timed sleep ---- */

static unsigned long proc_now_us(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? (ct * 1000000UL) / hz : 0;
}

void proc_setprio(int pid, int prio)
{
    if (pid < 0 || pid >= NPROC) return;
    prio = prio_clamp(prio);
    unsigned long d = irq_save();
    struct procent *p = &proctab[pid];
    if (p->state == PR_READY && p->qprio != prio) {
        /* Already queued: move it to the FIFO for its new priority, or the
         * bitmap and the new priority disagree and dispatch goes wrong. */
        rq_unlink(p);
        p->prio = prio;
        ready_push(p);
    } else {
        p->prio = prio;
    }
    irq_restore(d);
}

/* Block the caller until `us` microseconds elapse.  Like proc_block(), but the
 * timer tick (proc_timer_tick) readies it when its deadline passes and requests
 * a preemptive switch, so a high-priority periodic task wakes on time and (with
 * preemption on) preempts lower-priority compute.  Only callers of this sleep —
 * resident actors never do, so they keep running cooperatively. */
/* Microseconds until the earliest RT sleeper's deadline, capped to a periodic
 * floor (round-robin still happens with no sleepers) and a small minimum (a
 * just-passed deadline can't storm the timer).  Drives the tickless one-shot. */
#define PROC_TICK_FLOOR_US 1000UL
/* Hard minimum between one-shot fires.  Even if a stale sleeper or an aborted
 * test somehow keeps the next deadline at ~0, the timer can fire no faster than
 * this — so the IRQ path (a per-tick proctab scan) can never fully starve the
 * net/NULL process into a total hang; the worst case is a survivable slowdown.
 * 200 us = <=5000 IRQ/s; far above the ~150-230 us periods we actually use. */
#define PROC_TICK_MIN_US    200UL
/* O(1): the sleep list is sorted, so the earliest deadline is the head.
 * This used to scan all NPROC entries — and proc_timer_tick() scanned them
 * again in the same ISR, so every one-shot fire cost ~4096 uncached probes
 * at up to 5 kHz. */
unsigned long proc_next_delay_us(void)
{
    unsigned long best = PROC_TICK_FLOOR_US;
    unsigned long d = irq_save();
    if (sleep_head) {
        unsigned long now = proc_now_us();
        unsigned long w   = sleep_head->wake_at_us;
        unsigned long dd  = (w > now) ? (w - now) : 0;
        if (dd < best) best = dd;
    }
    irq_restore(d);
    return best < PROC_TICK_MIN_US ? PROC_TICK_MIN_US : best;
}

void proc_sleep_us(unsigned long us)
{
    extern void timer_arm_before_us(unsigned long);   /* tickless one-shot */
    unsigned long d = irq_save();
    struct procent *oldp = &proctab[currpid];

    stk_check(oldp);

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];
    if (newp == oldp) {
        /* Cannot happen for a PR_CURR process (it is not on the ready list),
         * but a NULLPROC caller with an empty ready list would otherwise
         * ctxsw into its own stale saved SP. */
        irq_restore(d);
        return;
    }

    oldp->wake_at_us = proc_now_us() + us;
    oldp->state = PR_SLEEP;
    sleep_insert(oldp);
    timer_arm_before_us(us);         /* fire precisely at this deadline if nearer */

    newp->state = PR_CURR;
    currpid = (int)(newp - proctab);
    g_ctxsw++;
    ctxsw(&oldp->sp, newp->sp);
    irq_restore(d);
}

/* Called from the timer IRQ handler (interrupts already masked): ready any
 * sleeper whose deadline has passed and request a reschedule so a woken
 * high-priority task runs immediately (via proc_preempt after EOI).
 * O(number actually woken) — the list is deadline-sorted, so we stop at the
 * first entry that is not due yet. */
void proc_timer_tick(void)
{
    unsigned long now = proc_now_us();
    int woke = 0;
    while (sleep_head && now >= sleep_head->wake_at_us) {
        struct procent *p = sleep_head;
        sleep_head = p->next;
        p->next  = 0;
        p->state = PR_READY;
        ready_push(p);
        g_dbg_tick_wakes++;
        woke = 1;
    }
    if (woke) g_resched_pending = 1;
}

/* Timer-driven preemption point: called from irq_dispatch_c after the IRQ is
 * EOI'd (so the next process can still receive timer IRQs).  Only preempts a
 * non-NULLPROC (i.e. a real process) when enabled and a tick is pending. */
void proc_preempt(void)
{
    g_dbg_pp_calls++;
    if (!g_preempt_on || !g_resched_pending) { g_dbg_pp_offgate++; return; }
    /* Suppress preemption while the actor pump runs (leave g_resched_pending set
     * so a tick isn't lost — the next tick after the pump leaves acts on it). */
    if (g_actor_pump) { g_dbg_pp_actorgate++; return; }
    g_resched_pending = 0;
    if (currpid != NULLPROC) { g_dbg_pp_fired++; proc_resched(); }
}

/* Reap a process that is blocked (PR_WAIT) — not on the ready list and
 * not running.  Frees its stack.  Used to clean up actor processes after
 * a one-shot run so the NPROC slots aren't exhausted. */
void proc_kill(int pid)
{
    if (pid <= 0 || pid >= NPROC || pid == currpid) return;
    unsigned long d = irq_save();
    struct procent *p = &proctab[pid];
    if (p->state == PR_FREE) { irq_restore(d); return; }
    ready_remove(p);                /* never leave a freed slot on the ready list */
    if (p->state == PR_SLEEP) sleep_remove(p);   /* nor on the deadline list */
    /* If we're reaping a process that holds the AIPL heap lock (e.g. an actor
     * killed mid-handler by ap_killall), release it — otherwise the dead owner
     * makes every later vheap user spin-yield forever and the app worker (and
     * thus all HTTP) wedges while ICMP keeps working. */
    if (g_aipl_owner == pid) { g_aipl_owner = -1; g_aipl_depth = 0; }
    if (p->stkbase) freemem(p->stkbase, p->stklen);
    p->stkbase = 0;
    p->state   = PR_FREE;
    irq_restore(d);
}

/* Block the current process: it leaves PR_CURR for PR_WAIT (so resched
 * will not re-ready it) and we switch to the next ready process, or back
 * to NULLPROC if none is ready.  Returns once proc_ready() re-readies us
 * and the scheduler picks us again. */
void proc_block(void)
{
    unsigned long d = irq_save();
    struct procent *oldp = &proctab[currpid];

    stk_check(oldp);

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];
    if (newp == oldp) {
        /* NULLPROC blocking with an empty ready list: there is nothing to
         * switch to and proc_ready() ignores pid 0, so parking here would be
         * permanent — and the ctxsw would restore our own stale SP.  Refuse. */
        irq_restore(d);
        return;
    }

    oldp->state = PR_WAIT;
    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    ctxsw(&oldp->sp, newp->sp);
    /* Resumes here when we are readied and ctxsw'd back into. */
    irq_restore(d);
}

/* ---------- AIPL vheap mutex (spin-yield) ----------
 * The AIPL runtime (cc interpreter) shares one non-reentrant value heap
 * (g_vheap in cc.c).  Under preemption, two vheap users (actors, the app
 * worker's http_build, shell cc) must never be mid-vheap-op at once.  This
 * lock serializes them: a contender spin-yields (proc_yield) so the holder
 * runs to its next release point.  It is acquired only around cc execution
 * bursts and released at every voluntary block point (ap_select), so the
 * cooperative actor pump (ap_run) still interleaves and a blocked holder
 * never wedges the heap.  The net process never takes this lock, so it can
 * always preempt a vheap user for low network latency.
 *
 * NULLPROC must not proc_block on this (proc_ready ignores pid 0), so the
 * lock spins with proc_yield — which always runs the holder — rather than
 * blocking.  Recursive (depth-counted) for safety against accidental
 * nesting; in practice depth stays 1.  (g_aipl_owner/g_aipl_depth are
 * declared near the top of this file so proc_kill can release a dead
 * owner's lock.) */

void aipl_lock(void)
{
    long spins = 0;
    for (;;) {
        unsigned long d = irq_save();
        if (g_aipl_owner < 0 || g_aipl_owner == currpid) {
            g_aipl_owner = currpid;
            g_aipl_depth++;
            irq_restore(d);
            return;
        }
        if (++spins > AIPL_SPIN_LIMIT) {
            /* Watchdog: the holder is wedged.  Record it and steal the lock so
             * the spinner (often the app worker) makes progress — keeps HTTP /
             * /lockstat alive so the culprit is visible. */
            int o = g_aipl_owner;
            g_lock_stuck_owner       = o;
            g_lock_stuck_owner_state = (o > 0 && o < NPROC) ? (int)proctab[o].state : -1;
            g_lock_stuck_by          = currpid;
            g_lock_timeouts++;
            g_aipl_owner = currpid;
            g_aipl_depth = 1;
            irq_restore(d);
            return;
        }
        irq_restore(d);
        proc_yield();                 /* let the holder run to its release point */
    }
}

void aipl_unlock(void)
{
    unsigned long d = irq_save();
    if (g_aipl_owner == currpid && --g_aipl_depth <= 0) {
        g_aipl_depth = 0;
        g_aipl_owner = -1;
    }
    irq_restore(d);
}

/* Release fully regardless of depth; returns the depth to restore later.
 * Used to drop the heap before a voluntary block (ap_select) and on the
 * let-it-crash path (where a longjmp skips the matching unlock). */
int aipl_unlock_all(void)
{
    unsigned long d = irq_save();
    int saved = 0;
    if (g_aipl_owner == currpid) { saved = g_aipl_depth; g_aipl_depth = 0; g_aipl_owner = -1; }
    irq_restore(d);
    return saved;
}

/* Reacquire to a previously-saved depth after waking from a block. */
void aipl_relock(int saved)
{
    if (saved <= 0) return;
    aipl_lock();                      /* spin-yield to depth 1 */
    unsigned long d = irq_save();
    g_aipl_depth = saved;             /* restore the full depth */
    irq_restore(d);
}

/* Unconditionally drop the lock — used by the fault handler so a process
 * that aborts while holding the heap doesn't wedge every other vheap user. */
void aipl_force_release(void)
{
    unsigned long d = irq_save();
    g_aipl_owner = -1;
    g_aipl_depth = 0;
    irq_restore(d);
}

/* Process voluntarily exits.  Marks slot free, picks next ready
 * (or NULLPROC if none), and ctxsw away — never returns. */
void proc_exit(void)
{
    irq_save();                 /* never returns -> no restore (each resume site
                                 * restores its own DAIF after ctxsw) */
    int me = currpid;
    stk_check(&proctab[me]);
    ready_remove(&proctab[me]); /* in case it was (re)queued — needs the state */
    if (proctab[me].state == PR_SLEEP) sleep_remove(&proctab[me]);
    proctab[me].state = PR_FREE;

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    /* Throw-away storage for the saved-SP write.  Nobody will read
     * proctab[me].sp again because we're PR_FREE. */
    static void *graveyard_sp;
    ctxsw(&graveyard_sp, newp->sp);

    /* Unreachable. */
    for (;;) __asm__ volatile ("wfe");
}
