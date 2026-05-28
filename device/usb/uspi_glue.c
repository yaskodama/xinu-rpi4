// device/usb/uspi_glue.c — USPi OS interface adapter.
//
// Implements the `uspios.h` contract on top of xinu-rpi4's kmalloc,
// generic timer (100 Hz tick), GIC IRQ dispatch and VC mailbox.
// USPi calls these from its DWC2 driver and HID class layers.
//
// IRQ mapping: USPi assumes IRQ 9 == "USB" (matching the legacy
// BCM2835 IRQ controller).  Pi 4's BCM2711 routes the DWC2 host
// controller to GIC SPI 105 instead, so we translate at the
// ConnectInterrupt() boundary.

#include "kmalloc.h"
#include "uart.h"
#include "irq.h"
#include "gic.h"
#include "timer.h"
#include "mbox.h"

/* USPi's IRQ number for the USB host controller (per BCM2835 docs). */
#define USPI_IRQ_USB_BCM2835   9
/* BCM2711 routes DWC2 USB host to GIC SPI 105. */
#define BCM2711_IRQ_USB_GIC    105

/* ---- stdarg without <stdarg.h> (we build -nostdinc) -------------- */
typedef __builtin_va_list va_list;
#define va_start(ap, l)   __builtin_va_start(ap, l)
#define va_end(ap)        __builtin_va_end(ap)
#define va_arg(ap, t)     __builtin_va_arg(ap, t)

/* ==== malloc / free ============================================== */

void *malloc(unsigned nSize)
{
    return kmalloc((unsigned long)nSize);
}

void free(void *pBlock)
{
    kfree(pBlock);
}

/* ==== Delay loops ================================================ */

static inline unsigned long cntfrq(void)
{
    unsigned long v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline unsigned long cntpct(void)
{
    unsigned long v;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void MsDelay(unsigned nMilliSeconds)
{
    unsigned long freq = cntfrq();
    unsigned long target = (freq / 1000UL) * (unsigned long)nMilliSeconds;
    unsigned long start = cntpct();
    while (cntpct() - start < target) { /* spin */ }
}

void usDelay(unsigned nMicroSeconds)
{
    unsigned long freq = cntfrq();
    unsigned long target = (freq / 1000000UL) * (unsigned long)nMicroSeconds;
    unsigned long start = cntpct();
    while (cntpct() - start < target) { /* spin */ }
}

/* ==== Kernel timer queue ========================================= */

typedef void TKernelTimerHandler(unsigned long hTimer, void *pParam, void *pContext);

#define USPI_TIMER_MAX 32

struct ktimer {
    int                  used;
    unsigned long        expire;     /* absolute tick (timer_ticks units) */
    TKernelTimerHandler *handler;
    void                *param;
    void                *context;
};

static struct ktimer ktimers[USPI_TIMER_MAX];

unsigned StartKernelTimer(unsigned             nHzDelay,
                          TKernelTimerHandler *pHandler,
                          void                *pParam,
                          void                *pContext)
{
    /* USPi configures uspios.h::HZ = 100 — same as our timer_ticks().
     * Look for a free slot (skip 0; uspios treats 0 as "no timer"). */
    for (int i = 1; i < USPI_TIMER_MAX; i++) {
        if (!ktimers[i].used) {
            ktimers[i].used    = 1;
            ktimers[i].expire  = timer_ticks() + nHzDelay;
            ktimers[i].handler = pHandler;
            ktimers[i].param   = pParam;
            ktimers[i].context = pContext;
            return (unsigned)i;
        }
    }
    return 0;
}

void CancelKernelTimer(unsigned hTimer)
{
    if (hTimer == 0 || hTimer >= USPI_TIMER_MAX) return;
    ktimers[hTimer].used = 0;
}

/* Called from the wm_run() frame loop (or any periodic site) to
 * fire timers whose deadline has arrived.  Cheap: <32 entries. */
void uspi_glue_tick(void)
{
    unsigned long now = timer_ticks();
    for (int i = 1; i < USPI_TIMER_MAX; i++) {
        if (ktimers[i].used && (long)(now - ktimers[i].expire) >= 0) {
            TKernelTimerHandler *fn = ktimers[i].handler;
            void *p = ktimers[i].param;
            void *c = ktimers[i].context;
            ktimers[i].used = 0;
            if (fn) fn((unsigned long)i, p, c);
        }
    }
}

/* ==== IRQ wiring (USPi <-> our irq_dispatch) ====================== */

typedef void TInterruptHandler(void *pParam);

void ConnectInterrupt(unsigned             nIRQ,
                      TInterruptHandler   *pHandler,
                      void                *pParam)
{
    unsigned gic_irq = (nIRQ == USPI_IRQ_USB_BCM2835)
                       ? BCM2711_IRQ_USB_GIC
                       : nIRQ;
    /* connect_interrupt() takes irq_handler_t = void (*)(void *) which
     * matches USPi's TInterruptHandler signature exactly. */
    connect_interrupt(gic_irq, (irq_handler_t)pHandler, pParam);
    gic_enable_irq(gic_irq);
}

/* ==== VC mailbox property tags ==================================== */

int SetPowerStateOn(unsigned nDeviceId)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;                /* total size */
    buf[1] = 0;                 /* request code */
    buf[2] = 0x00028001U;       /* set power state */
    buf[3] = 8;                 /* value buffer size */
    buf[4] = 0;                 /* req/resp code */
    buf[5] = nDeviceId;
    buf[6] = 3;                 /* on | wait for stable */
    buf[7] = 0;                 /* end tag */
    /* uspios: returns 1 on success, 0 on failure. */
    return mbox_call(buf) == 0 ? 1 : 0;
}

int GetMACAddress(unsigned char Buffer[6])
{
    /* xinu-rpi4 doesn't surface Ethernet — USPi only needs this
     * when SMSC951x / LAN7800 are linked, which we keep disabled
     * for the keyboard/mouse-only profile.  Return 0 (failure). */
    for (int i = 0; i < 6; i++) Buffer[i] = 0;
    return 0;
}

/* ==== LogWrite (printf-lite) ====================================== */

static void put_dec_u(unsigned long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(tmp[n]);
}

static void put_hex_u(unsigned long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) {
        unsigned long d = v & 0xF;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    while (n--) uart_putc(tmp[n]);
}

void LogWrite(const char *pSource, unsigned Severity, const char *pMessage, ...)
{
    (void)Severity;
    uart_puts("[uspi:");
    uart_puts(pSource);
    uart_puts("] ");

    va_list ap;
    va_start(ap, pMessage);
    const char *p = pMessage;
    while (*p) {
        if (*p != '%') { uart_putc(*p++); continue; }
        p++;
        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; }

        switch (*p) {
            case 's': { const char *s = va_arg(ap, const char *); uart_puts(s ? s : "(null)"); break; }
            case 'd':
            case 'i': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0) { uart_putc('-'); put_dec_u((unsigned long)-v); }
                else        put_dec_u((unsigned long)v);
                break;
            }
            case 'u': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned int);
                put_dec_u(v);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned int);
                put_hex_u(v);
                break;
            }
            case 'p': { void *v = va_arg(ap, void *);
                        uart_puts("0x"); put_hex_u((unsigned long)v); break; }
            case 'c': { int v = va_arg(ap, int); uart_putc((char)v); break; }
            case '%': { uart_putc('%'); break; }
            default:  { uart_putc('%'); if (*p) uart_putc(*p); break; }
        }
        if (*p) p++;
    }
    va_end(ap);
    uart_puts("\n");
}

/* ==== Debug helpers (NDEBUG-gated in uspios.h) ==================== */

void uspi_assertion_failed(const char *pExpr, const char *pFile, unsigned nLine)
{
    uart_puts("\n[uspi assert] ");
    uart_puts(pFile);
    uart_puts(":");
    put_dec_u((unsigned long)nLine);
    uart_puts(": ");
    uart_puts(pExpr);
    uart_puts("\nHalted.\n");
    for (;;) __asm__ volatile ("wfe");
}

void DebugHexdump(const void *pBuffer, unsigned nBufLen, const char *pSource)
{
    (void)pBuffer; (void)nBufLen; (void)pSource;
}
