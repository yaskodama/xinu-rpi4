// include/irq.h — IRQ dispatch API.
//
// The S1a vector table routes "Current EL with SPx — IRQ" (offset
// 0x280) into the assembly stub `irq_entry`, which saves caller-
// saved registers and calls `irq_dispatch_c()` here.  The C side
// reads the GIC IAR, looks up the registered handler, calls it,
// then writes EOIR.
//
// Handler registration uses connect_interrupt() — the same shape
// USPi's uspios.h expects, so the future USPi adapter can forward
// to this API one-for-one.

#ifndef XINU_RPI4_IRQ_H
#define XINU_RPI4_IRQ_H

typedef void (*irq_handler_t)(void *arg);

/* Register `fn` to be called when interrupt `irq` fires.  `arg` is
 * passed unchanged to the handler.  Pass fn=0 to detach.  irq is
 * the full GIC ID (PPI = 16..31, SPI = 32..).  Caller is responsible
 * for enabling the source via gic_enable_irq() afterwards. */
void connect_interrupt(unsigned irq, irq_handler_t fn, void *arg);

/* Called from irq_entry (asm) on every IRQ taken at EL1.  Reads
 * GIC IAR, dispatches, EOIs.  Reentry-safe with respect to nested
 * IRQs only insofar as the GIC enforces priority ordering — we
 * don't unmask DAIF.I inside the handler. */
void irq_dispatch_c(void);

/* Unmask IRQs at the CPU (clears DAIF.I).  Call this only after
 * connecting at least one handler and configuring the GIC + the
 * actual interrupt source — otherwise a spurious IRQ falls into
 * an unconnected slot and the default handler halts. */
static inline void irq_enable_all(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

static inline void irq_disable_all(void)
{
    __asm__ volatile ("msr daifset, #2" ::: "memory");
}

#endif /* XINU_RPI4_IRQ_H */
