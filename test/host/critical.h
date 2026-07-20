/* test/host/critical.h — host build stub for include/critical.h.
 *
 * The real one masks DAIF.I with AArch64 inline asm, which the host
 * compiler can't assemble.  Host tests are single-threaded, so a no-op
 * pair is faithful enough for testing the data-structure logic.
 * This directory is placed FIRST on the include path by the Makefile so
 * it shadows include/critical.h; every other header resolves normally. */

#ifndef XINU_RPI4_CRITICAL_H
#define XINU_RPI4_CRITICAL_H

static inline unsigned long irq_save(void)      { return 0UL; }
static inline void irq_restore(unsigned long d) { (void)d; }

#endif /* XINU_RPI4_CRITICAL_H */
