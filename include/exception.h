// include/exception.h — AArch64 exception / IRQ entry points.
//
// Phase S1a installs a 2-KiB-aligned exception vector table into
// VBAR_EL1.  Until S1b/c/d wire up the GIC + generic timer, all
// 16 vector slots route to a `dump-and-halt` handler that prints
// ESR_EL1 / FAR_EL1 / ELR_EL1 on the UART (and via uart_putc,
// also the shell window) so a stray data abort can no longer
// stall the kernel silently behind early_paint_diagnostic().

#ifndef XINU_RPI5_EXCEPTION_H
#define XINU_RPI5_EXCEPTION_H

/* Program VBAR_EL1 with the address of `exception_vector_table`.
 * Idempotent; safe to call from the boot path or after a remap. */
void exception_init(void);

#endif /* XINU_RPI5_EXCEPTION_H */
