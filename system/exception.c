// system/exception.c — default handlers for the S1a vector table.
//
// Every slot in exception_vectors.S routes here.  Each handler reads the
// standard EL1 status registers (ESR / FAR / ELR / SP / SPSR) and emits a
// short banner on the UART (which also feeds the wm shell-window scrollback).
//
// Recovery: serial is unreadable on this host and a WFE halt freezes the
// framebuffer too, so a fault used to be invisible *and* take the whole box
// down (no ICMP).  Instead we now RECORD the fault into globals (surfaced by
// the /fault HTTP route) and, for a synchronous exception, keep the box alive
// so it can be diagnosed remotely: drop the AIPL heap lock (the aborting
// process may have held it), enable preemption + IRQs, and spin.  The 100 Hz
// timer then preempts this stuck context and the scheduler runs the net
// process / app worker, so HTTP (and /fault) keep working.  The faulting
// process is effectively abandoned in the spin loop.

#include "uart.h"

extern void aipl_force_release(void);   /* system/proc.c */
extern void proc_set_preempt(int on);   /* system/proc.c */

/* Captured fault state — surfaced by the /fault diagnostic route. */
volatile unsigned long g_fault_count;
volatile unsigned long g_fault_esr, g_fault_far, g_fault_elr, g_fault_spsr, g_fault_sp;

static void puts_hex64(unsigned long v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned long nyb = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[18] = 0;
    uart_puts(buf);
}

static void capture(const char *kind)
{
    unsigned long esr, far, elr, sp, spsr;
    __asm__ volatile ("mrs %0, esr_el1"  : "=r"(esr));
    __asm__ volatile ("mrs %0, far_el1"  : "=r"(far));
    __asm__ volatile ("mrs %0, elr_el1"  : "=r"(elr));
    __asm__ volatile ("mrs %0, spsr_el1" : "=r"(spsr));
    __asm__ volatile ("mov %0, sp"       : "=r"(sp));

    g_fault_esr = esr; g_fault_far = far; g_fault_elr = elr;
    g_fault_spsr = spsr; g_fault_sp = sp; g_fault_count++;

    uart_puts("\n[EXC] ");
    uart_puts(kind);
    uart_puts("\n  ESR_EL1  = "); puts_hex64(esr);  uart_puts("\n");
    uart_puts(  "  FAR_EL1  = "); puts_hex64(far);  uart_puts("\n");
    uart_puts(  "  ELR_EL1  = "); puts_hex64(elr);  uart_puts("\n");
    uart_puts(  "  SPSR_EL1 = "); puts_hex64(spsr); uart_puts("\n");
    uart_puts(  "  SP       = "); puts_hex64(sp);   uart_puts("\n");
}

/* Keep the box alive after a sync fault so /fault can be read over HTTP.
 * The aborting process is abandoned in this spin; the timer preempts it. */
static void recover_spin(void)
{
    aipl_force_release();                          /* don't leave the heap locked */
    proc_set_preempt(1);                           /* let the timer preempt this context */
    __asm__ volatile ("msr daifclr, #2" ::: "memory");   /* unmask IRQs */
    for (;;) __asm__ volatile ("wfi");             /* timer preempts us -> other procs run */
}

static void dump_and_halt(const char *kind)
{
    capture(kind);
    uart_puts("Halted.\n");
    for (;;) __asm__ volatile ("wfe");
}

__attribute__((used))
void sync_handler_default(void)
{
    capture("sync exception");
    uart_puts("Recovering (box stays up; read /fault).\n");
    recover_spin();
}

__attribute__((used))
void irq_handler_default(void)    { dump_and_halt("IRQ (no dispatcher wired yet)"); }

__attribute__((used))
void fiq_handler_default(void)    { dump_and_halt("FIQ"); }

__attribute__((used))
void serror_handler_default(void) { dump_and_halt("SError"); }
