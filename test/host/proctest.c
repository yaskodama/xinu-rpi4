/* test/host/proctest.c — host-side regression tests for the scheduler in
 * system/proc.c.
 *
 *     make -C test/host run
 *
 * proc.c's scheduling logic — who gets picked, queue ordering, sleep
 * deadlines, slot lifecycle — is plain data-structure code.  Only the final
 * ctxsw() actually touches the CPU, so stubbing that out lets everything
 * above it be tested natively.  With a no-op ctxsw, proc_resched() behaves as
 * if we were switched away and immediately back, which is exactly what we
 * want: the selection is observable in `currpid` and the queue state.
 *
 * What this guards:
 *
 *  1. THE HEADLINE BUG.  proc_resched() used to pop the best ready process
 *     BEFORE requeueing the running one, so the running process's priority
 *     never entered the comparison — a timer tick handed a prio-50 RT task's
 *     CPU to a prio-5 hog.  Test `resched_respects_current_priority` fails
 *     against the old ordering.
 *  2. Priority dispatch and round-robin among equals.
 *  3. proc_setprio() on an already-queued process must move it between
 *     FIFOs, or the ready bitmap and the priority disagree.
 *  4. Sleep deadlines are honoured in order and the O(1) head lookup agrees.
 *  5. Dead/exited slots never stay queued (a stale entry means the next
 *     dispatch ctxsw's into freed memory).
 *  6. The stack canary notices an overflow.
 */

#include <stdio.h>
#include <stdlib.h>

/* --- stubs for the parts that need real hardware ---------------------- */

/* ctxsw normally saves callee-saved registers and swaps SP.  Here we only
 * need it to be a no-op that plausibly writes back a saved SP. */
static unsigned long ctxsw_calls;
void ctxsw(void **old_sp, void *new_sp)
{
    (void)new_sp;
    ctxsw_calls++;
    if (old_sp) *old_sp = (void *)0x1000;   /* non-NULL sentinel */
}

/* Address-taken only (stored in the initial stack frame). */
void proc_entry_trampoline(void) { }

/* The tickless one-shot; irrelevant to the queue logic. */
static unsigned long last_arm_us;
void timer_arm_before_us(unsigned long us) { last_arm_us = us; }

#include "../../mem/memory.c"
#include "../../system/proc.c"

/* --- harness ---------------------------------------------------------- */

static int fails;

static void fail(const char *tag, const char *msg, long v)
{
    printf("FAIL [%s] %s (%ld)\n", tag, msg, v);
    fails++;
}

static void expect_eq(const char *tag, const char *what, long got, long want)
{
    if (got != want) {
        printf("FAIL [%s] %s: got %ld, want %ld\n", tag, what, got, want);
        fails++;
    }
}

static unsigned char arena[1 << 20] __attribute__((aligned(16)));

static void reset_world(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + sizeof arena);
    proc_init();
    ctxsw_calls = 0;
}

static void dummy_entry(void) { }

static int spawn(const char *name, int prio)
{
    int pid = proc_create(dummy_entry, 4096, name);
    if (pid > 0) proc_setprio(pid, prio);
    return pid;
}

/* Consistency check run after every test: the bitmap, the per-priority FIFOs
 * and rq_count must agree, and every queued process must be PR_READY. */
static void check_queues(const char *tag)
{
    int counted = 0;
    for (int p = 0; p <= PROC_PRIO_MAX; p++) {
        int nonempty = (rq_head[p] != 0);
        int bit      = (rq_bitmap >> p) & 1ULL;
        if (nonempty != bit) { fail(tag, "bitmap disagrees with FIFO at prio", p); return; }
        struct procent *prev = 0;
        for (struct procent *c = rq_head[p]; c; c = c->next) {
            if (c->state != PR_READY) { fail(tag, "queued process is not PR_READY", c - proctab); return; }
            if (c->qprio != p)        { fail(tag, "process in the wrong FIFO", c - proctab); return; }
            if (c->prev != prev)      { fail(tag, "prev link broken", c - proctab); return; }
            prev = c;
            if (++counted > NPROC)    { fail(tag, "cycle in ready queue", p); return; }
        }
        if (rq_tail[p] != prev) { fail(tag, "tail pointer wrong at prio", p); return; }
    }
    if (counted != rq_count) { fail(tag, "rq_count disagrees with walk", counted); return; }

    /* The sleep list must be sorted and hold only PR_SLEEP processes. */
    unsigned long prev_deadline = 0;
    int n = 0;
    for (struct procent *c = sleep_head; c; c = c->next) {
        if (c->state != PR_SLEEP)          { fail(tag, "sleep list holds non-sleeper", c - proctab); return; }
        if (c->wake_at_us < prev_deadline) { fail(tag, "sleep list not sorted", c - proctab); return; }
        prev_deadline = c->wake_at_us;
        if (++n > NPROC)                   { fail(tag, "cycle in sleep list", 0); return; }
    }
}

/* --- tests ------------------------------------------------------------ */

/* Highest priority wins, regardless of creation order. */
static void test_priority_dispatch(void)
{
    reset_world();
    int lo  = spawn("lo",  1);
    int mid = spawn("mid", 5);
    int hi  = spawn("hi",  50);
    (void)lo; (void)mid;

    proc_resched();                       /* from NULLPROC (prio 0) */
    expect_eq("prio-dispatch", "selected pid", currpid, hi);
    check_queues("prio-dispatch");
}

/* Equal priorities rotate in FIFO order. */
static void test_round_robin(void)
{
    reset_world();
    int a = spawn("a", 1);
    int b = spawn("b", 1);
    int c = spawn("c", 1);

    proc_resched();  expect_eq("round-robin", "1st", currpid, a);
    proc_resched();  expect_eq("round-robin", "2nd", currpid, b);
    proc_resched();  expect_eq("round-robin", "3rd", currpid, c);
    proc_resched();  expect_eq("round-robin", "wraps", currpid, a);
    check_queues("round-robin");
}

/* THE HEADLINE BUG.  A running high-priority task must not be displaced by a
 * lower-priority ready one.  The old proc_resched popped before requeueing
 * the runner, so the runner's priority never competed and this switched. */
static void test_resched_respects_current_priority(void)
{
    reset_world();
    int hi  = spawn("rt",  50);
    int hog = spawn("hog", 5);

    proc_resched();                       /* NULLPROC -> rt (highest) */
    expect_eq("current-prio", "rt runs", currpid, hi);

    unsigned long sw = ctxsw_calls;
    proc_resched();                       /* a timer tick lands here */
    expect_eq("current-prio", "rt still runs", currpid, hi);
    expect_eq("current-prio", "no context switch happened",
              (long)(ctxsw_calls - sw), 0);
    if (proctab[hog].state != PR_READY)
        fail("current-prio", "hog left the ready queue", proctab[hog].state);
    if (proctab[hi].state != PR_CURR)
        fail("current-prio", "rt is not PR_CURR after the no-op resched",
             proctab[hi].state);
    check_queues("current-prio");
}

/* ...but a strictly higher-priority ready task DOES displace the runner. */
static void test_higher_priority_preempts(void)
{
    reset_world();
    int lo = spawn("lo", 5);
    proc_resched();
    expect_eq("preempt", "lo runs", currpid, lo);

    int hi = spawn("rt", 50);             /* arrives while lo runs */
    proc_resched();
    expect_eq("preempt", "rt takes over", currpid, hi);
    if (proctab[lo].state != PR_READY)
        fail("preempt", "displaced task not requeued", proctab[lo].state);
    check_queues("preempt");
}

/* Changing the priority of an already-queued process must move it. */
static void test_setprio_requeues(void)
{
    reset_world();
    int a = spawn("a", 1);
    int b = spawn("b", 1);

    proc_setprio(b, 60);                  /* b is queued at prio 1 */
    check_queues("setprio");
    proc_resched();
    expect_eq("setprio", "raised process runs first", currpid, b);
    (void)a;
    check_queues("setprio");
}

/* Priorities outside the bitmap must be clamped, not silently corrupt it. */
static void test_prio_clamp(void)
{
    reset_world();
    int p = spawn("wild", 9999);
    expect_eq("clamp", "clamped high", proctab[p].prio, PROC_PRIO_MAX);
    proc_setprio(p, -5);
    expect_eq("clamp", "clamped low", proctab[p].prio, 0);
    check_queues("clamp");
}

/* Sleepers are woken in deadline order, and the O(1) head lookup agrees. */
static void test_sleep_ordering(void)
{
    reset_world();
    /* Build the sleep list by hand: proc_sleep_us would ctxsw away, and our
     * stub ctxsw returns immediately, which is not what a sleeper does. */
    int a = spawn("a", 1), b = spawn("b", 1), c = spawn("c", 1);
    struct procent *pa = &proctab[a], *pb = &proctab[b], *pc = &proctab[c];
    ready_remove(pa); ready_remove(pb); ready_remove(pc);
    pa->state = PR_SLEEP; pa->wake_at_us = 3000;
    pb->state = PR_SLEEP; pb->wake_at_us = 1000;
    pc->state = PR_SLEEP; pc->wake_at_us = 2000;
    sleep_insert(pa); sleep_insert(pb); sleep_insert(pc);
    check_queues("sleep");

    if (sleep_head != pb) fail("sleep", "earliest deadline not at head", 0);

    /* Wake only those that are due.  proc_timer_tick uses the real counter,
     * so drive the list directly with a deadline sweep of our own. */
    unsigned long now = 1500;
    int woke = 0;
    while (sleep_head && now >= sleep_head->wake_at_us) {
        struct procent *p = sleep_head;
        sleep_head = p->next; p->next = 0;
        p->state = PR_READY; ready_push(p);
        woke++;
    }
    expect_eq("sleep", "woke exactly the due sleeper", woke, 1);
    if (proctab[b].state != PR_READY) fail("sleep", "b not readied", proctab[b].state);
    if (sleep_head != pc) fail("sleep", "next deadline wrong", 0);
    check_queues("sleep");
}

/* proc_ready() on a sleeper must take it off the deadline list first —
 * otherwise it is on two lists whose links share the same field. */
static void test_early_wake_leaves_sleep_list(void)
{
    reset_world();
    int a = spawn("a", 1);
    struct procent *pa = &proctab[a];
    ready_remove(pa);
    pa->state = PR_SLEEP; pa->wake_at_us = 5000;
    sleep_insert(pa);

    proc_ready(a);
    if (sleep_head != 0) fail("early-wake", "still on the sleep list", 0);
    if (proctab[a].state != PR_READY) fail("early-wake", "not readied", proctab[a].state);
    check_queues("early-wake");
}

/* A killed process must leave every queue. */
static void test_kill_dequeues(void)
{
    reset_world();
    int a = spawn("a", 1);
    int b = spawn("b", 1);
    int before = rq_count;
    proc_kill(a);
    expect_eq("kill", "ready count dropped", rq_count, before - 1);
    expect_eq("kill", "slot freed", proctab[a].state, PR_FREE);
    check_queues("kill");

    proc_resched();
    expect_eq("kill", "dispatch skips the dead slot", currpid, b);
    check_queues("kill");
}

/* A free/exited slot must never be queueable. */
static void test_ready_rejects_dead(void)
{
    reset_world();
    int a = spawn("a", 1);
    proc_kill(a);
    int before = rq_count;
    proc_ready(a);                        /* stale wake for a dead pid */
    expect_eq("ready-dead", "PR_FREE not queued", rq_count, before);

    proctab[a].state = PR_TERM;
    proc_ready(a);
    expect_eq("ready-dead", "PR_TERM not queued", rq_count, before);
    check_queues("ready-dead");
}

/* Double-ready must not queue a process twice (a self-linked node makes
 * dispatch return the running process). */
static void test_double_ready(void)
{
    reset_world();
    int a = spawn("a", 1);
    struct procent *pa = &proctab[a];
    ready_remove(pa);
    pa->state = PR_WAIT;

    proc_ready(a);
    int after_first = rq_count;
    proc_ready(a);
    expect_eq("double-ready", "second ready ignored", rq_count, after_first);
    check_queues("double-ready");
}

/* The canary must notice a process that ran off the bottom of its stack. */
static void test_stack_canary(void)
{
    reset_world();
    int a = spawn("a", 1);
    proc_resched();                       /* a is now current */
    expect_eq("canary", "a runs", currpid, a);

    unsigned long before = proc_stk_bad_count();
    /* Simulate an overflow: something wrote past the low end of the stack. */
    *(volatile unsigned long *)proctab[a].stkbase = 0;
    proc_resched();
    if (proc_stk_bad_count() != before + 1)
        fail("canary", "overflow not detected", (long)proc_stk_bad_count());
    expect_eq("canary", "culprit recorded", proc_stk_bad_pid(), a);
    /* Re-armed, so a second switch doesn't double-count the same event. */
    unsigned long mid = proc_stk_bad_count();
    proc_resched();
    expect_eq("canary", "re-armed", (long)(proc_stk_bad_count() - mid), 0);
    check_queues("canary");
}

/* Stacks come out of getmem() and are used as AArch64 SPs. */
static void test_stack_alignment(void)
{
    reset_world();
    for (int i = 0; i < 32; i++) {
        int pid = proc_create(dummy_entry, 2048 + i * 8, "s");
        if (pid < 0) { fail("stk-align", "spawn failed", i); return; }
        if ((unsigned long)proctab[pid].stkbase % 16)
            fail("stk-align", "stack base not 16-byte aligned", i);
        if ((unsigned long)proctab[pid].sp % 16)
            fail("stk-align", "initial SP not 16-byte aligned", i);
        if (proctab[pid].stklen < 2048)
            fail("stk-align", "stack below the IRQ-frame floor", (long)proctab[pid].stklen);
    }
    check_queues("stk-align");
}

/* Churn: spawn, run, kill at mixed priorities; invariants must hold. */
static void test_churn(void)
{
    reset_world();
    int pids[64];
    for (int i = 0; i < 64; i++) pids[i] = 0;

    srand(4242);
    for (int it = 0; it < 20000; it++) {
        int i = rand() % 64;
        if (pids[i]) {
            if (pids[i] != currpid) { proc_kill(pids[i]); pids[i] = 0; }
        } else {
            pids[i] = spawn("x", rand() % (PROC_PRIO_MAX + 1));
        }
        if (rand() % 3 == 0) proc_resched();
        if (!(it % 211)) { check_queues("churn"); if (fails) return; }
    }
    check_queues("churn-end");
}

int main(void)
{
    test_priority_dispatch();
    test_round_robin();
    test_resched_respects_current_priority();
    test_higher_priority_preempts();
    test_setprio_requeues();
    test_prio_clamp();
    test_sleep_ordering();
    test_early_wake_leaves_sleep_list();
    test_kill_dequeues();
    test_ready_rejects_dead();
    test_double_ready();
    test_stack_canary();
    test_stack_alignment();
    test_churn();

    if (fails) { printf("\n=== %d FAILURE(S) ===\n", fails); return 1; }
    printf("\n=== all scheduler tests passed ===\n");
    return 0;
}
