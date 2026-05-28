// include/gic.h — Pi 4 BCM2711 GIC-400 distributor / CPU interface.
//
// The GIC-400 is an ARM IP block on the BCM2711.  Layout (per the
// BCM2711 peripherals datasheet, §6):
//
//   GICD (distributor)   0xFF841000
//   GICC (CPU interface) 0xFF842000
//
// Interrupt ID classes (GIC-v2):
//   0   .. 15   SGI  Software-generated     (per-core)
//   16  .. 31   PPI  Private peripheral     (per-core, e.g. timers)
//   32  .. 1019 SPI  Shared peripheral      (global)
//
// Useful Pi 4 IDs:
//   PPI 30  = ARM generic timer (CNTPNS / physical non-secure)
//   SPI 105 = USB host (DWC2) — VC_IRQ + 9 in Pi 4 layout
//
// API contract: SPI / PPI numbers, never raw GICD register bits.
// gic_ack() returns the IAR value (low 10 bits = active IRQ ID);
// pass the same value to gic_eoi() to retire.

#ifndef XINU_RPI4_GIC_H
#define XINU_RPI4_GIC_H

#ifdef GIC_BASE   /* Pi 4 only — PI4_CFLAGS supplies the value */

/* Initialise the distributor + CPU interface.  Disables and
 * acks any pending interrupts, sets priority mask to "all", and
 * enables group 0 (= non-secure for BCM2711 in EL1). */
void gic_init(void);

/* Enable / disable interrupt `irq` (SPI or PPI ID). */
void gic_enable_irq(unsigned irq);
void gic_disable_irq(unsigned irq);

/* Read the Interrupt Acknowledge Register.  Low 10 bits = the
 * active interrupt ID; high bits identify the requesting core
 * (for SGIs).  Returns 1023 if there's no pending interrupt
 * ("spurious"). */
unsigned int gic_ack(void);

/* Retire interrupt by writing the IAR value back to EOIR. */
void gic_eoi(unsigned int iar);

#else  /* GIC_BASE not defined — Pi 5 / QEMU */

static inline void gic_init(void) {}
static inline void gic_enable_irq(unsigned irq) { (void)irq; }
static inline void gic_disable_irq(unsigned irq) { (void)irq; }
static inline unsigned int gic_ack(void) { return 1023u; }
static inline void gic_eoi(unsigned int iar) { (void)iar; }

#endif

#endif /* XINU_RPI4_GIC_H */
