// system/exception.c — default handlers for the S1a vector table.
//
// Every slot in exception_vectors.S routes here.  Each handler
// reads the standard EL1 status registers (ESR / FAR / ELR /
// SP / SPSR), emits a short banner on the UART (which also
// feeds the wm shell-window scrollback), and then halts the
// core via WFE.  No register save / restore — recovery isn't
// supported at this phase; the goal is just to make a stray
// fault *visible* so it can be diagnosed.

#include "uart.h"

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

static void dump_and_halt(const char *kind)
{
    unsigned long esr, far, elr, sp, spsr;
    __asm__ volatile ("mrs %0, esr_el1"  : "=r"(esr));
    __asm__ volatile ("mrs %0, far_el1"  : "=r"(far));
    __asm__ volatile ("mrs %0, elr_el1"  : "=r"(elr));
    __asm__ volatile ("mrs %0, spsr_el1" : "=r"(spsr));
    __asm__ volatile ("mov %0, sp"       : "=r"(sp));

    uart_puts("\n[EXC] ");
    uart_puts(kind);
    uart_puts("\n  ESR_EL1  = "); puts_hex64(esr);  uart_puts("\n");
    uart_puts(  "  FAR_EL1  = "); puts_hex64(far);  uart_puts("\n");
    uart_puts(  "  ELR_EL1  = "); puts_hex64(elr);  uart_puts("\n");
    uart_puts(  "  SPSR_EL1 = "); puts_hex64(spsr); uart_puts("\n");
    uart_puts(  "  SP       = "); puts_hex64(sp);   uart_puts("\n");
    uart_puts("Halted.\n");

    for (;;) __asm__ volatile ("wfe");
}

__attribute__((used))
void sync_handler_default(void)   { dump_and_halt("sync exception"); }

__attribute__((used))
void irq_handler_default(void)    { dump_and_halt("IRQ (no dispatcher wired yet)"); }

__attribute__((used))
void fiq_handler_default(void)    { dump_and_halt("FIQ"); }

__attribute__((used))
void serror_handler_default(void) { dump_and_halt("SError"); }
