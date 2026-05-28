// kernel/shell.h — minimal interactive shell for the boot UART.
//
// Modelled after davidxyz/xinuPi's shell.c REPL (read line → tokenise
// → look up in a centry table → dispatch), but stripped to fit the
// bare-metal Round-1 context — no threads, no tty driver, just direct
// PL011 UART I/O and a tiny static command table.

#ifndef XINU_RPI4_SHELL_H
#define XINU_RPI4_SHELL_H

#define SHELL_BUFLEN   128
#define SHELL_MAXTOK    16

/* Never returns — for use when the kernel falls back to UART-only
 * mode (no HDMI framebuffer). */
void shell_main(void);

/* Dispatch a single shell line (in-place tokenisation; `line` must
 * be writable and at most SHELL_BUFLEN chars).  Used by shellwin
 * to drive the shell from the wm frame loop without a blocking
 * uart_getline().  Returns 0 on dispatch / unknown command, never
 * blocks. */
int  shell_dispatch_line(char *line);

#endif /* XINU_RPI4_SHELL_H */
