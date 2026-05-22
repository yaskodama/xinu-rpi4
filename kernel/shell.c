// kernel/shell.c — bare-metal Xinu-style xsh on the boot UART.
//
// Follows the davidxyz/xinuPi shell.c shape: a `centry` table maps
// command name → handler.  We don't have thread, tty, file
// descriptors or printf yet on Pi 5 AArch64, so each handler talks
// directly to the UART through the helpers in uart.h.
//
// The REPL loop:
//   1. print SHELL_PROMPT
//   2. uart_getline() — blocks for a CR/LF-terminated line
//   3. tokenise on ASCII whitespace
//   4. linear search the command table; on hit, call the handler
//   5. on miss, print "?command-not-found" and loop
//
// Built-in commands:
//   help                 list the registered commands
//   echo  <words...>     print the rest of the line verbatim
//   hello                friendly greeting (smoke marker)
//   mem                  link-script symbols: __bss_start, __bss_end, _end
//   peek  <hex_addr>     read 32-bit word at MMIO address (default base
//                        BCM2712 — try `peek 0x107d001018` for UART_FR)
//   uptime               printed since this is bare metal: stub "?", later
//                        bound to the generic timer in phase S1
//   reboot               watchdog-driven reset via RP1 (stub — needs
//                        more work; for now just spins)
//
// Designed so phase S0 (thread switch) and S1 (clock IRQ) can later
// replace these stubs without touching the dispatch code.

#include "uart.h"
#include "shell.h"

/* Linker-script symbols (from kernel/link.ld). */
extern unsigned long __bss_start;
extern unsigned long __bss_end;
extern unsigned long _end;

/* ---------- helpers (no libc available) ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Render an unsigned long in hex (0x… prefix, lowercase). */
static void puts_hex(unsigned long v)
{
    char buf[2 + 16 + 1];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        unsigned long nyb = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[18] = 0;
    uart_puts(buf);
}

/* Parse a hex string like "0x107d001018" or "107d001018" → unsigned long.
 * Returns 0 on parse error (caller checks the original token if it cares). */
static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        unsigned long d;
        if (c >= '0' && c <= '9') d = (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned long)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

/* ---------- command implementations ---------- */

typedef int (*cmd_fn)(int argc, char **argv);

struct centry {
    const char *name;
    const char *help;
    cmd_fn      fn;
};

static const struct centry commandtab[];   /* forward */

static int cmd_help(int argc, char **argv)
{
    int i;
    (void)argc; (void)argv;
    uart_puts("available commands:\n");
    for (i = 0; commandtab[i].name; i++) {
        uart_puts("  ");
        uart_puts(commandtab[i].name);
        /* pad to ~10 columns */
        int pad = 10 - str_len(commandtab[i].name);
        while (pad-- > 0) uart_putc(' ');
        uart_puts(commandtab[i].help);
        uart_puts("\n");
    }
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        uart_puts(argv[i]);
        if (i + 1 < argc) uart_putc(' ');
    }
    uart_puts("\n");
    return 0;
}

static int cmd_hello(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("hello from Xinu on Raspberry Pi 5 (BCM2712, AArch64)\n");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("__bss_start = "); puts_hex((unsigned long)&__bss_start); uart_puts("\n");
    uart_puts("__bss_end   = "); puts_hex((unsigned long)&__bss_end);   uart_puts("\n");
    uart_puts("_end        = "); puts_hex((unsigned long)&_end);        uart_puts("\n");
    return 0;
}

static int cmd_peek(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("usage: peek <hex_addr>\n");
        return 1;
    }
    unsigned long addr = parse_hex(argv[1]);
    if (addr == 0) {
        uart_puts("peek: bad address\n");
        return 1;
    }
    unsigned int v = *(volatile unsigned int *)addr;
    uart_puts("["); puts_hex(addr); uart_puts("] = ");
    puts_hex((unsigned long)v);
    uart_puts("\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Generic timer plumbing arrives in phase S1.  For now read
     * CNTPCT_EL0 directly so the user gets *some* signal of life. */
    unsigned long cnt;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
    uart_puts("cntpct_el0 = "); puts_hex(cnt); uart_puts("\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("reboot: RP1 watchdog not wired up yet — spinning in WFE.\n");
    uart_puts("        (power-cycle the board to recover)\n");
    for (;;) __asm__ volatile ("wfe");
}

static const struct centry commandtab[] = {
    { "help",   "list the commands",                       cmd_help   },
    { "echo",   "echo the remaining words back",           cmd_echo   },
    { "hello",  "smoke marker — say hello",                cmd_hello  },
    { "mem",    "show __bss_start / __bss_end / _end",     cmd_mem    },
    { "peek",   "peek <hex_addr> — read 32-bit MMIO word", cmd_peek   },
    { "uptime", "raw CNTPCT_EL0 (generic timer)",          cmd_uptime },
    { "reboot", "stub — spins until power-cycle",          cmd_reboot },
    { "?",      "alias for help",                          cmd_help   },
    { 0, 0, 0 }
};

/* ---------- main REPL ---------- */

static int tokenise(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p && n < SHELL_MAXTOK) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tok[n++] = p;
        /* skip word */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

void shell_main(void)
{
    static char line[SHELL_BUFLEN];
    static char *tok[SHELL_MAXTOK];

    uart_puts("type `help` for the command list.\n");

    for (;;) {
        uart_puts("xinu-pi5$ ");
        int n = uart_getline(line, SHELL_BUFLEN);
        if (n <= 0) continue;

        int ntok = tokenise(line, tok);
        if (ntok == 0) continue;

        const struct centry *e;
        for (e = commandtab; e->name; e++) {
            if (str_eq(tok[0], e->name)) {
                e->fn(ntok, tok);
                goto next;
            }
        }
        uart_puts("?command-not-found: ");
        uart_puts(tok[0]);
        uart_puts(" (try `help`)\n");
    next: ;
    }
}
