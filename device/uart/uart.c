// kernel/uart.c — bare-metal PL011 UART0 driver for Raspberry Pi 4.
//
// This is the first I/O Xinu has available on Pi 4; everything else
// (the framebuffer mailbox, the eMMC, GENET network) comes later.
// We rely on the firmware having already enabled UART0 via config.txt
// (enable_uart=1 + dtparam=uart0=on) which locks the UART clock to a
// stable 48 MHz reference, so we only need to:
//
//   1. Disable the UART so we can reprogram safely.
//   2. Configure GPIO14 (TXD) and GPIO15 (RXD) as ALT0 (PL011).
//      The firmware's enable_uart=1 has already done the muxing for us,
//      so we can skip GPIO programming on the first cut and add it
//      in a later phase if the cable misbehaves.
//   3. Program 115200/8N1: integer baud divisor 26, fractional 3
//      ((48e6) / (16 * 115200) = 26.0416…).
//   4. Re-enable the UART.
//
// Once initialised, putc spins on the TX-FIFO-full flag and writes
// one byte to DR.  No interrupts here — this is the absolute earliest
// I/O channel and the kernel proper hasn't even set up an exception
// table yet.

#include "uart.h"
#include "video.h"
#include "shellwin.h"

/* BCM2711 (Pi 4) peripheral base 0xFE000000 + PL011 UART0 offset 0x201000.
 *
 * Overridable at compile time via -DUART0_BASE=0xNN so the same
 * source builds for QEMU `virt` (PL011 at 0x09000000) without
 * touching the real-hardware default.  The firmware (enable_uart=1)
 * does the GPIO ALT muxing for us on Pi 4 + QEMU virt. */
#ifndef UART0_BASE
#define UART0_BASE   0xFE201000UL
#endif

#define UART_DR      (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART_FR      (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART_IBRD    (*(volatile unsigned int *)(UART0_BASE + 0x24))
#define UART_FBRD    (*(volatile unsigned int *)(UART0_BASE + 0x28))
#define UART_LCRH    (*(volatile unsigned int *)(UART0_BASE + 0x2C))
#define UART_CR      (*(volatile unsigned int *)(UART0_BASE + 0x30))
#define UART_ICR     (*(volatile unsigned int *)(UART0_BASE + 0x44))

#define FR_RXFE      (1u << 4)   /* RX FIFO empty  */
#define FR_TXFF      (1u << 5)   /* TX FIFO full   */
#define LCRH_FEN     (1u << 4)   /* enable FIFOs   */
#define LCRH_WLEN_8  (3u << 5)   /* 8-bit words    */
#define CR_UARTEN    (1u << 0)
#define CR_TXE       (1u << 8)
#define CR_RXE       (1u << 9)

/* ---- RX diagnostics (for the /uartrx HTTP bisect route) -------------
 * Every byte actually pulled out of the RX FIFO (by uart_poll_char, the
 * single live drainer in shellwin_step, or by the blocking uart_getc)
 * bumps g_uart_rx_count and is stashed in a 16-byte ring.  This lets the
 * HTTP gateway answer the one question serial can't answer about itself:
 * "are the user's keystrokes physically reaching the Pi's RX at all?"
 *   count climbs while typing  -> bytes arrive, bug is in dispatch (SW)
 *   count stays 0 while typing  -> bytes never arrive (wiring/baud/mux) */
volatile unsigned int  g_uart_rx_count = 0;
static volatile unsigned char g_uart_rx_ring[16];
static volatile unsigned int  g_uart_rx_head = 0;

static inline void uart_rx_note(unsigned char c)
{
    g_uart_rx_count++;
    g_uart_rx_ring[g_uart_rx_head & 15] = c;
    g_uart_rx_head++;
}

void uart_init(void)
{
#ifdef GPIO_BASE
    /* Pi 4 (BCM2711): route GPIO14 (TXD0) and GPIO15 (RXD0) to ALT0 so
     * the PL011 we drive at UART0_BASE actually reaches header pins
     * 8/10.  Relying on the firmware's enable_uart muxing is unreliable
     * on Pi 4 — the PL011 is often tied to Bluetooth while the
     * mini-UART (ttyS0) is what gets placed on the header, in which
     * case our PL011 output would never appear on the cable.  Force it. */
    {
        volatile unsigned int *gpfsel1 =
            (volatile unsigned int *)(GPIO_BASE + 0x04);   /* GPIO10..19 */
        unsigned int v = *gpfsel1;
        v &= ~((7u << 12) | (7u << 15));   /* clear FSEL14, FSEL15        */
        v |=  ((4u << 12) | (4u << 15));   /* ALT0 (0b100) = TXD0/RXD0    */
        *gpfsel1 = v;
        /* Pull-none on 14/15 (BCM2711 PUP_PDN_CNTRL_REG0: 2 bits each,
         * GPIO14 = bits 29:28, GPIO15 = bits 31:30). */
        volatile unsigned int *pud0 =
            (volatile unsigned int *)(GPIO_BASE + 0xE4);
        unsigned int p = *pud0;
        p &= ~((3u << 28) | (3u << 30));
        *pud0 = p;
        __asm__ volatile ("dsb sy" ::: "memory");
    }
#endif

    /* 1. Disable while we reprogram. */
    UART_CR = 0;

    /* Clear any pending interrupts left from the bootrom. */
    UART_ICR = 0x7FF;

    /* 3. 115200 baud from a 48 MHz UART clock.
     *    divisor = 48e6 / (16 * 115200) = 26.0416…
     *    fractional part * 64 + 0.5 = 3
     */
    UART_IBRD = 26;
    UART_FBRD = 3;

    /* 8N1 with FIFOs enabled. */
    UART_LCRH = LCRH_FEN | LCRH_WLEN_8;

    /* 4. Re-enable TX + RX. */
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;

    /* Drain any bytes the firmware / QEMU stdio pumped in before we
     * finished reprogramming — otherwise piped-stdin smoke tests
     * (and the rare USB-serial chatter) lose their first byte to
     * the LCRH-write FIFO reset.  Harmless on a real cable. */
    while (!(UART_FR & FR_RXFE)) {
        (void)UART_DR;
    }
}

/* Re-program the PL011 baud divisor for a true 115200 given the ACTUAL
 * UART reference clock `clk` (Hz), as reported by the firmware mailbox.
 * The boot-time uart_init() assumes 48 MHz; on Pi 4 the firmware often
 * leaves UARTCLK at some other rate, garbling the cable.  Call this once
 * (from the /uartclk route) after boot to fix it without boot-time mbox
 * risk.  divisor = clk / (16*115200); IBRD = int, FBRD = round(frac*64). */
void uart_rebaud(unsigned int clk)
{
    unsigned int want = 115200u;
    unsigned int ibrd, fbrd, div64;
    if (!clk) return;
    /* div * 64 = clk*4 / want  (since 16*want, *64 => *4/want) */
    div64 = (unsigned int)(((unsigned long long)clk * 4ULL) / want);
    ibrd = div64 >> 6;
    fbrd = div64 & 0x3F;
    if (ibrd == 0) return;             /* clk too low — refuse, keep current */
    /* Drain TX, disable, reprogram, re-enable (PL011 requires LCRH rewrite
     * after IBRD/FBRD to latch the new divisor). */
    while (!(UART_FR & (1u<<7))) { }   /* wait TXFE (FIFO empty) bit7 */
    UART_CR = 0;
    UART_IBRD = ibrd;
    UART_FBRD = fbrd;
    UART_LCRH = LCRH_FEN | LCRH_WLEN_8;
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;
}

/* ---- remote-shell output capture (used by the /shell HTTP route) ----
 * When capture is on, every byte written to the console is also appended to
 * a buffer so the HTTP gateway can run a shell command and return its output.
 * The console (UART/HDMI/wm) still receives the bytes — capture is a tee. */
static char uart_cap[8192];
static int  uart_cap_n  = 0;
static int  uart_cap_on = 0;
void uart_capture_begin(void) { uart_cap_n = 0; uart_cap_on = 1; }
int  uart_capture_end(char *dst, int max)
{
    int n = uart_cap_n, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) dst[i] = uart_cap[i];
    uart_cap_on = 0;
    return n;
}

void uart_putc(char c)
{
    if (uart_cap_on && uart_cap_n < (int)sizeof uart_cap)
        uart_cap[uart_cap_n++] = c;        /* tee into the capture buffer */

    /* Busy-wait until the TX FIFO has room. */
    while (UART_FR & FR_TXFF) {
        /* spin */
    }
    UART_DR = (unsigned int)(unsigned char)c;

    /* Mirror to the HDMI console once video_init() has succeeded
     * (after that point screen_putc is safe; before, it's a no-op).
     * This is the whole point of the framebuffer driver — keeping
     * a parallel log channel in case the UART cable is silent on
     * this particular Pi 4 board. */
    screen_putc(c);

    /* Also feed the wm shell window's scrollback ring.  Safe to
     * call before shellwin_init() — it just drops the byte then. */
    shellwin_record_char(c);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

/* Hex print without printf — useful inside ISRs and low-level probe code
 * that can't allocate or take locks.  Always emits exactly 8 hex digits
 * for 32-bit clarity (high bits dropped if value > 0xFFFFFFFF). */
void uart_puthex(unsigned long v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        uart_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

char uart_getc(void)
{
    /* Busy-wait until the RX FIFO has something. */
    while (UART_FR & FR_RXFE) {
        /* spin */
    }
    unsigned char c = (unsigned char)(UART_DR & 0xFF);
    uart_rx_note(c);
    return (char)c;
}

int uart_poll_char(void)
{
    if (UART_FR & FR_RXFE) return -1;
    unsigned char c = (unsigned char)(UART_DR & 0xFF);
    uart_rx_note(c);
    return (int)c;
}

/* Format a one-line snapshot of the RX hardware + drain state for the
 * /uartrx HTTP route.  All the PL011 (and Pi-4 GPIO) register macros are
 * file-local, so the formatting lives here and the gateway just calls it.
 * Reads are non-destructive (FR/CR/GPFSEL are status regs; we do NOT touch
 * DR here so we never steal a byte from shellwin_step's drain). */
int uart_rx_debug(char *buf, int max)
{
    static const char hex[] = "0123456789ABCDEF";
    int p = 0;
    unsigned int fr = UART_FR, cr = UART_CR;
    unsigned int cnt = g_uart_rx_count, head = g_uart_rx_head;
#define PUT(ch)   do { if (p < max - 1) buf[p++] = (ch); } while (0)
#define PUTS(str) do { const char *_s=(str); while(*_s) PUT(*_s++); } while (0)
#define PUTHEX(v) do { unsigned int _v=(v); for(int _i=7;_i>=0;_i--) PUT(hex[(_v>>(_i*4))&0xF]); } while (0)
    PUTS("rx_count=");  { char d[12]; int n=0,v=cnt; if(!v)d[n++]='0'; while(v){d[n++]='0'+v%10;v/=10;} while(n)PUT(d[--n]); }
    PUTS(" fr=0x");  PUTHEX(fr);
    PUTS(" (RXFE=");  PUT(fr & FR_RXFE ? '1':'0'); PUT(')');
    PUTS(" cr=0x");  PUTHEX(cr);
    PUTS(" (RXE=");  PUT(cr & CR_RXE ? '1':'0'); PUTS(" TXE="); PUT(cr & CR_TXE ? '1':'0'); PUT(')');
#ifdef GPIO_BASE
    {
        unsigned int gpfsel1 = *(volatile unsigned int *)(GPIO_BASE + 0x04);
        /* FSEL15 = bits 17:15 (RXD0), FSEL14 = bits 14:12 (TXD0); ALT0 = 0b100. */
        PUTS(" gpfsel1=0x"); PUTHEX(gpfsel1);
        PUTS(" (fsel14="); PUT('0'+((gpfsel1>>12)&7));
        PUTS(" fsel15=");  PUT('0'+((gpfsel1>>15)&7)); PUT(')');
    }
#endif
    PUTS(" last16=");
    {
        unsigned int seen = head < 16 ? head : 16;
        unsigned int start = head - seen;
        for (unsigned int i = 0; i < seen; i++) {
            unsigned char c = g_uart_rx_ring[(start + i) & 15];
            PUTHEX(c); PUT(i + 1 < seen ? ',' : ' ');
        }
        if (!seen) PUTS("(none)");
    }
    PUTS("\n");
#undef PUT
#undef PUTS
#undef PUTHEX
    if (p < max) buf[p] = 0;
    return p;
}

/* Line editor: blocking, echo + backspace + DEL.  CR/LF terminates. */
int uart_getline(char *buf, int max)
{
    int n = 0;
    if (max <= 0) {
        return 0;
    }
    while (n < max - 1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            buf[n] = 0;
            return n;
        }
        if (c == 0x08 || c == 0x7F) {  /* BS / DEL */
            if (n > 0) {
                n--;
                /* Visually erase: backspace, space, backspace. */
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {   /* printable */
            buf[n++] = c;
            uart_putc(c);              /* echo */
        }
        /* Silently swallow other control chars. */
    }
    buf[n] = 0;
    return n;
}
