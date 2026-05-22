// kernel/shell.h — minimal interactive shell for the boot UART.
//
// Modelled after davidxyz/xinuPi's shell.c REPL (read line → tokenise
// → look up in a centry table → dispatch), but stripped to fit the
// bare-metal Round-1 context — no threads, no tty driver, just direct
// PL011 UART I/O and a tiny static command table.

#ifndef XINU_RPI5_SHELL_H
#define XINU_RPI5_SHELL_H

#define SHELL_BUFLEN   128
#define SHELL_MAXTOK    16

/* Never returns. */
void shell_main(void);

#endif /* XINU_RPI5_SHELL_H */
