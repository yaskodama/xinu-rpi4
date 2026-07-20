// device/timer/timer.c — ARM generic timer, 100 Hz periodic tick.
//
// IRQ source: PPI 30 (CNTPNS — physical non-secure timer).  At
// EL1 non-secure on BCM2711 this is the only timer we have
// direct access to; CNTV / CNTHP need EL2.
//
// Programming model:
//   - CNTFRQ_EL0  read-only counter frequency in Hz
//   - CNTP_TVAL_EL0  signed countdown register; IRQ fires when ≤ 0
//   - CNTP_CTL_EL0   bit0 = ENABLE, bit1 = IMASK, bit2 = ISTATUS
//
// On every IRQ we reload CNTP_TVAL_EL0 with the same interval to
// make it periodic.  No drift compensation — good enough for a
// 100 Hz scheduler / USPi `StartKernelTimer` substrate.

#include "timer.h"
#include "irq.h"
#include "gic.h"
#include "proc.h"

#define TIMER_IRQ_PPI 30

static unsigned long          timer_interval;
static unsigned long          timer_freq;      /* CNTFRQ_EL0 (ticks/sec) */
static volatile unsigned long tick_count;

static inline void cntp_set_tval(unsigned long v)
{
    __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(v));
}

static inline long cntp_get_tval(void)
{
    long v;
    __asm__ volatile ("mrs %0, cntp_tval_el0" : "=r"(v));
    return v;   /* signed countdown: ticks remaining (negative once fired) */
}

static inline unsigned long us_to_ticks(unsigned long us)
{
    return (timer_freq * us) / 1000000UL;
}

/* Tickless one-shot: arm the timer to fire `us` from now if that is SOONER than
 * what is currently pending, so a new nearer RT deadline gets sub-tick wakeup.
 * Called (IRQs masked) from proc_sleep_us(). */
void timer_arm_before_us(unsigned long us)
{
    long want = (long)us_to_ticks(us);
    if (want < cntp_get_tval()) cntp_set_tval((unsigned long)(want > 1 ? want : 1));
}

static inline void cntp_set_ctl(unsigned long v)
{
    __asm__ volatile ("msr cntp_ctl_el0, %0" : : "r"(v));
}

static inline unsigned long cntfrq(void)
{
    unsigned long v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void timer_irq_handler(void *arg)
{
    (void)arg;
    tick_count++;
    proc_timer_tick();              /* wake due RT sleepers (may set resched_pending) */
    proc_resched_request();         /* ask for a preemptive switch (acted on after EOI) */
    /* Tickless one-shot: fire again at the next RT deadline, or after the
     * periodic floor (round-robin preemption) if no sleeper is nearer. */
    cntp_set_tval(us_to_ticks(proc_next_delay_us()));
}

void timer_init(void)
{
    unsigned long freq = cntfrq();
    timer_freq     = freq;
    timer_interval = freq / TIMER_HZ;

    /* Mask + disable while we configure so a partial state can't
     * fire an unexpected IRQ. */
    cntp_set_ctl(2);                /* IMASK=1, ENABLE=0 */
    cntp_set_tval(timer_interval);

    connect_interrupt(TIMER_IRQ_PPI, timer_irq_handler, 0);
    gic_enable_irq(TIMER_IRQ_PPI);

    /* ENABLE=1, IMASK=0 — IRQ now pending in CNTP, will fire as
     * soon as the CPU unmasks DAIF.I (caller's responsibility). */
    cntp_set_ctl(1);
}

unsigned long timer_ticks(void)
{
    return tick_count;
}
