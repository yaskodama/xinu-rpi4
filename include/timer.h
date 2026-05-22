// include/timer.h — ARM generic timer driver, 100 Hz tick.
//
// Uses CNTP (physical timer, non-secure) via PPI 30 on BCM2711.
// CNTFRQ_EL0 holds the timer base frequency (set by firmware,
// typically 54 MHz on Pi 4).  We program CNTP_TVAL_EL0 with
// (CNTFRQ_EL0 / HZ) so an interrupt fires every (1/HZ) seconds,
// and the IRQ handler reloads the same value to make it periodic.

#ifndef XINU_RPI5_TIMER_H
#define XINU_RPI5_TIMER_H

#define TIMER_HZ 100

/* Configure CNTFRQ_EL0 / CNTP_TVAL_EL0 / CNTP_CTL_EL0, register
 * the IRQ handler via connect_interrupt(), and enable PPI 30 in
 * the GIC.  Caller is responsible for unmasking DAIF.I (see
 * irq_enable_all() in irq.h) once everything else is initialised. */
void timer_init(void);

/* Monotonically increasing tick counter (one tick per 1/HZ). */
unsigned long timer_ticks(void);

#endif /* XINU_RPI5_TIMER_H */
