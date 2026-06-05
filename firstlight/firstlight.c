// firstlight/firstlight.c — minimal Raspberry Pi 5 (BCM2712) bring-up.
//
// HDMI-FIRST bring-up.  The primary "is the kernel alive?" signal is the
// HDMI display, because the dev's USB-serial adapter cannot change baud
// on macOS (see firstlight/NEXT_SESSION_PI5.md).  So this build proves
// liveness purely on screen:
//
//   1. A full-screen RED -> GREEN -> BLUE colour cycle.  A static
//      firmware splash cannot change colour, so a moving colour cycle
//      already proves the kernel is executing and the mailbox framebuffer
//      is ours.
//   2. A banner.
//   3. A CONTINUOUS HEARTBEAT — a blinking square plus an incrementing
//      on-screen frame counter.  This is the crucial bit: a one-shot
//      banner cannot tell "alive and looping" from "drew once then hung".
//      A counter that keeps ticking can.
//
// Serial is SECONDARY and routed to the BCM2712 dedicated DEBUG UART at
// 0x107D001000 (the 3-pin JST-SH header between the micro-HDMI ports).
// That UART is PCIe-INDEPENDENT and on-SoC, so writing to it never
// data-aborts even with no cable attached and even when config.txt does
// not bring PCIe up.  (The RP1 UART0 at 0x1F00030000 on GPIO14/15 needs
// pciex4_reset=0 and WOULD data-abort otherwise — we deliberately avoid
// it on the HDMI route.)  Pair this build with sdcard/config_pi5min.txt.
//
// We do NOT reprogram the UART baud — the firmware (enable_uart=1 +
// uart_2ndstage=1) already set the debug UART to 115200 8N1.
//
// boot.S has already dropped EL2->EL1, set up the stack and zeroed BSS,
// then branched here as kernel_main().

extern const unsigned char font8x8[96][8];           /* device/video/...   */

/* ---- VideoCore mailbox (property channel 8) ----------------------- */
/* MBOX_BASE is injected by the Makefile (-DMBOX_BASE=0x107C00B880UL for
 * the Pi 5 / BCM2712).  We poke the mailbox HERE rather than via
 * device/mbox/mbox.c so the whole HDMI bring-up experiment lives in one
 * file: the key unknown on the Pi 5 is the BUS address the VideoCore
 * expects for our request buffer.  On the Pi 4 the raw ARM physical
 * address works; the Pi 1-3 (and the u-boot reference's phys_to_bus)
 * need the 0xC0000000 uncached alias.  We don't know which the Pi 5
 * wants, so fb_init() tries several conventions in one boot. */
#ifndef MBOX_BASE
#define MBOX_BASE 0x107C00B880UL   /* BCM2712 VideoCore mailbox (Pi 5) */
#endif
#define MBOX_REG(off)  (*(volatile unsigned int *)(MBOX_BASE + (off)))
#define MBOX_STATUS    MBOX_REG(0x18)
#define MBOX_WRITE     MBOX_REG(0x20)
#define MBOX_READ      MBOX_REG(0x00)
#define MB_FULL        (1u << 31)
#define MB_EMPTY       (1u << 30)
#define MB_TIMEOUT     100000000UL

/* Send request buffer `buf` on channel 8, forming the GPU-visible
 * address as (phys & ~0xF) | bus_or.  Returns 0 on a success response. */
static int fl_mbox(volatile unsigned int *buf, unsigned int bus_or)
{
    unsigned int msg = (((unsigned int)(unsigned long)buf & ~0xFu) | bus_or) | 8u;
    unsigned long t;
    int a;

    for (t = 0; t < MB_TIMEOUT; t++)
        if (!(MBOX_STATUS & MB_FULL)) break;
    if (t == MB_TIMEOUT) return -1;

    MBOX_WRITE = msg;

    for (a = 0; a < 32; a++) {
        for (t = 0; t < MB_TIMEOUT; t++)
            if (!(MBOX_STATUS & MB_EMPTY)) break;
        if (t == MB_TIMEOUT) return -1;
        if ((MBOX_READ & 0xFu) == 8u)
            return (buf[1] == 0x80000000u) ? 0 : -1;   /* response code */
    }
    return -1;
}

/* ---- Dedicated DEBUG UART (PCIe-independent, always safe) ---------- */
#define SERIAL_BASE  0x107D001000UL
#define UREG(off)  (*(volatile unsigned int *)(SERIAL_BASE + (off)))
#define UART_DR    UREG(0x00)
#define UART_FR    UREG(0x18)
#define FR_RXFE    (1u << 4)
#define FR_TXFF    (1u << 5)

static void put_c(char c)
{
    while (UART_FR & FR_TXFF) { }
    UART_DR = (unsigned int)(unsigned char)c;
}

static void put_s(const char *s)
{
    while (*s) {
        if (*s == '\n') put_c('\r');
        put_c(*s++);
    }
}

static void put_hex32(unsigned int v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    for (i = 28; i >= 0; i -= 4) put_c(hx[(v >> i) & 0xF]);
}

/* ---- HDMI framebuffer (VideoCore mailbox) ------------------------- */
#define FB_W   640u
#define FB_H   480u
#define SCALE  2u

static volatile unsigned char *fb;
static unsigned int fb_pitch, fb_w, fb_h, fb_bpp;

static void fb_pixel(unsigned int x, unsigned int y, unsigned int rgb)
{
    volatile unsigned char *p = fb + (unsigned long)y * fb_pitch
                                   + (unsigned long)x * fb_bpp;
    if (fb_bpp == 4) {
        *(volatile unsigned int *)p = rgb;
    } else {
        unsigned int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
        *(volatile unsigned short *)p =
            (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

static void fb_clear(unsigned int rgb)
{
    unsigned int x, y;
    for (y = 0; y < fb_h; y++)
        for (x = 0; x < fb_w; x++)
            fb_pixel(x, y, rgb);
}

static void fb_fill(unsigned int x0, unsigned int y0,
                    unsigned int w, unsigned int h, unsigned int rgb)
{
    unsigned int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            fb_pixel(x0 + x, y0 + y, rgb);
}

static void fb_char(unsigned int px, unsigned int py, char ch, unsigned int rgb)
{
    int r, c;
    const unsigned char *g;
    if (ch < 0x20 || ch > 0x7e) ch = ' ';
    g = font8x8[(int)ch - 0x20];
    for (r = 0; r < 8; r++) {
        unsigned char bits = g[r];
        for (c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                unsigned int sx, sy;
                for (sy = 0; sy < SCALE; sy++)
                    for (sx = 0; sx < SCALE; sx++)
                        fb_pixel(px + (unsigned)c * SCALE + sx,
                                 py + (unsigned)r * SCALE + sy, rgb);
            }
        }
    }
}

static void fb_string(unsigned int px, unsigned int py, const char *s, unsigned int rgb)
{
    while (*s) {
        fb_char(px, py, *s++, rgb);
        px += 8 * SCALE;
    }
}

/* Render an unsigned decimal at (px,py), painting a fixed-width field so
 * later (shorter) numbers fully overwrite earlier (longer) ones. */
static void fb_decimal(unsigned int px, unsigned int py, unsigned int v,
                       unsigned int rgb, unsigned int bg)
{
    char buf[11];
    int n = 0, i;
    fb_fill(px, py, 10u * 8u * SCALE, 8u * SCALE, bg);   /* clear field */
    if (v == 0) buf[n++] = '0';
    while (v > 0 && n < 10) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = n - 1; i >= 0; i--) {
        fb_char(px, py, buf[i], rgb);
        px += 8 * SCALE;
    }
}

/* Which bus-address convention actually got a framebuffer (for display). */
static unsigned int fb_bus_or_used;

static int fb_try(unsigned int bus_or)
{
    static volatile unsigned int __attribute__((aligned(16))) mb[36];
    unsigned int addr = 0, pitch = 0, w = FB_W, h = FB_H;
    int i = 0, j;

    mb[i++] = 0;
    mb[i++] = 0;
    mb[i++] = 0x48003; mb[i++] = 8; mb[i++] = 8; mb[i++] = FB_W; mb[i++] = FB_H;
    mb[i++] = 0x48004; mb[i++] = 8; mb[i++] = 8; mb[i++] = FB_W; mb[i++] = FB_H;
    mb[i++] = 0x48009; mb[i++] = 8; mb[i++] = 8; mb[i++] = 0;    mb[i++] = 0;
    mb[i++] = 0x48005; mb[i++] = 4; mb[i++] = 4; mb[i++] = 32;
    mb[i++] = 0x48006; mb[i++] = 4; mb[i++] = 4; mb[i++] = 1;
    mb[i++] = 0x40001; mb[i++] = 8; mb[i++] = 8; mb[i++] = 4096; mb[i++] = 0;
    mb[i++] = 0x40008; mb[i++] = 4; mb[i++] = 4; mb[i++] = 0;
    mb[i++] = 0;
    mb[0] = (unsigned int)(i * 4);

    if (fl_mbox(mb, bus_or) < 0) return -1;

    for (j = 2; j < i - 1; ) {
        unsigned int tag = mb[j];
        unsigned int valsz = mb[j + 1];
        if (tag == 0) break;
        if (tag == 0x48003) { w = mb[j + 3]; h = mb[j + 4]; }
        if (tag == 0x40001) { addr = mb[j + 3]; }
        if (tag == 0x40008) { pitch = mb[j + 3]; }
        j += 3 + (int)(valsz >> 2);
    }

    if (addr == 0 || pitch == 0) return -1;

    addr &= 0x3FFFFFFFu;
    fb = (volatile unsigned char *)(unsigned long)addr;
    fb_pitch = pitch;
    fb_w = w;
    fb_h = h;
    fb_bpp = (w ? pitch / w : 4);
    if (fb_bpp == 0) fb_bpp = 4;
    fb_bus_or_used = bus_or;
    return 0;
}

static int fb_init(void)
{
    /* Try the most likely Pi 5 bus conventions in one boot:
     *   0xC0000000 = legacy VC4 uncached alias (Pi 1-3, u-boot phys_to_bus)
     *   0x40000000 = cached alias
     *   0x00000000 = raw ARM physical (works on the Pi 4) */
    static const unsigned int conv[3] = { 0xC0000000u, 0x40000000u, 0x00000000u };
    int k;
    for (k = 0; k < 3; k++)
        if (fb_try(conv[k]) == 0) return 0;
    return -1;
}

static void delay(unsigned long n)
{
    volatile unsigned long i;
    for (i = 0; i < n; i++) {
        __asm__ volatile ("" ::: "memory");
    }
}

#define DELAY_FRAME  40000000UL   /* ~ full-screen colour, human-visible */
#define DELAY_BEAT    8000000UL   /* heartbeat cadence (~few/sec)        */

/* ------------------------------------------------------------------- */
void kernel_main(void)
{
    unsigned long el;
    unsigned int tick = 0;
    int have_fb;

    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    el >>= 2;

    /* ---- HDMI: the primary, PCIe-independent liveness channel ------- */
    have_fb = (fb_init() == 0);

    /* Mirror the FB-allocation result to SERIAL so we can tell "mailbox
     * returned no framebuffer" (have_fb=0) from "FB allocated but HDMI shows
     * no signal" (have_fb=1) — the HDMI itself can't report its own failure. */
    put_s("\nfb_init: have_fb=");  put_c((char)('0' + have_fb));
    put_s(" bus=0x");   put_hex32(fb_bus_or_used);
    put_s(" w=0x");     put_hex32(fb_w);
    put_s(" h=0x");     put_hex32(fb_h);
    put_s(" pitch=0x"); put_hex32(fb_pitch);
    put_s(" base=0x");  put_hex32((unsigned int)(unsigned long)fb);
    put_s("\n");

    if (have_fb) {
        unsigned int row;

        fb_clear(0xFF0000); delay(DELAY_FRAME);        /* red   */
        fb_clear(0x00FF00); delay(DELAY_FRAME);        /* green */
        fb_clear(0x0000FF); delay(DELAY_FRAME);        /* blue  */

        fb_clear(0x001020);                            /* dark navy */
        row = 40;
        fb_string(40, row, "Xinu Pi 5 first-light", 0x00FF66);          row += 12 * SCALE;
        fb_string(40, row, "HDMI framebuffer is LIVE", 0xFFFFFF);        row += 10 * SCALE;
        fb_string(40, row, "kernel is running on the Pi 5", 0xFFFF00);   row += 10 * SCALE;
        fb_char(40, row, (char)('0' + (int)(el & 7)), 0xAAAAAA);
        fb_string(40 + 8 * SCALE, row, " = CurrentEL", 0xAAAAAA);        row += 12 * SCALE;
        /* which mailbox bus convention worked (top nibble: C/4/0) */
        fb_string(40, row, "mbox bus = 0x", 0xAAAAAA);
        {
            unsigned int nib = (fb_bus_or_used >> 28) & 0xF;
            char hc = (char)(nib < 10 ? '0' + nib : 'A' + (nib - 10));
            fb_char(40 + 13 * 8 * SCALE, row, hc, 0xFFFFFF);
        }                                                               row += 14 * SCALE;
        fb_string(40, row, "heartbeat:", 0x66CCFF);
    }

    /* ---- SERIAL on the dedicated debug UART (0x107D001000) ----------- */
    /* PCIe-independent on-SoC UART: safe to write even with no cable. */
    put_s("\n\n");
    put_s("==================================\n");
    put_s("  Xinu Pi 5 first-light kernel\n");
    put_s("==================================\n");
    put_s("serial = dedicated DEBUG UART @ 0x107D001000\n");
    put_s("CurrentEL = ");
    put_c((char)('0' + (int)(el & 7)));
    put_s("\n\n");
    put_s("HDMI shows a blinking heartbeat + counter.\n");
    put_s("If you can read this on the debug UART, serial works too.\n\n");

    /* ---- Continuous heartbeat: prove the kernel keeps running -------- */
    for (;;) {
        tick++;

        if (have_fb) {
            /* blinking square: green on even ticks, dark on odd */
            unsigned int beat = (tick & 1) ? 0x103040 : 0x00FF66;
            fb_fill(40 + 11u * 8u * SCALE, 40 + 56u * SCALE,
                    10u * SCALE, 10u * SCALE, beat);
            /* ticking counter just right of the square */
            fb_decimal(40 + 14u * 8u * SCALE, 40 + 56u * SCALE,
                       tick, 0xFFFFFF, 0x001020);
        }

        put_c('.');                                    /* serial heartbeat */
        if ((tick % 50u) == 0) put_s("\n");

        /* echo any keystrokes (debug UART RX FIFO; empty if no cable) */
        if (!(UART_FR & FR_RXFE)) {
            char c = (char)(UART_DR & 0xff);
            put_c(c);
            if (c == '\r') put_c('\n');
        }

        delay(DELAY_BEAT);
    }
}
