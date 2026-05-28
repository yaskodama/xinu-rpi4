// cc/cc.c — on-device C compiler driver + runtime.
//
// Pipeline: read a source file from the VFS -> cc_lex -> cc_parse ->
// cc_codegen (AArch64 machine code into an executable buffer) -> flush
// caches -> call the compiled main() in place (JIT) -> print its return
// value.  Compiled code reaches the kernel through cc_resolve_extern
// (print/putchar/puts/actor_send), which is the seam the AIPL->C output
// will use to drive the Xinu actor runtime.

#include "ccpriv.h"
#include "cc.h"
#include "vfs.h"
#include "uart.h"
#include "kmalloc.h"
#include "actor.h"

/* ---------- arena allocator (whole-program lifetime) ---------- */

static char          *g_arena;
static unsigned long  g_acap, g_apos;
static char           g_panic[64];     /* safe sink once we OOM */

static int arena_init(unsigned long cap)
{
    g_arena = (char *)kmalloc(cap);
    g_acap  = g_arena ? cap : 0;
    g_apos  = 0;
    return g_arena != 0;
}
static void arena_free(void)
{
    if (g_arena) kfree(g_arena);
    g_arena = 0; g_acap = g_apos = 0;
}

void *cc_alloc(unsigned long n)
{
    n = (n + 15) & ~15UL;                  /* 16-byte align */
    if (!g_arena || g_apos + n > g_acap) { cc_error("compiler arena exhausted"); return g_panic; }
    void *p = g_arena + g_apos;
    g_apos += n;
    /* zero the block */
    for (unsigned long i = 0; i < n; i++) ((char *)p)[i] = 0;
    return p;
}

/* ---------- error reporting ---------- */

static int  g_err;
static char g_errbuf[128];

static void copy_msg(const char *m)
{
    int i = 0;
    while (m[i] && i < (int)sizeof(g_errbuf) - 1) { g_errbuf[i] = m[i]; i++; }
    g_errbuf[i] = 0;
}

void cc_error(const char *msg)  { if (g_err) return; g_err = 1; copy_msg(msg); }

void cc_errorc(const char *msg, char c)
{
    if (g_err) return;
    g_err = 1;
    int i = 0;
    while (msg[i] && i < (int)sizeof(g_errbuf) - 5) { g_errbuf[i] = msg[i]; i++; }
    g_errbuf[i++] = ' '; g_errbuf[i++] = '\''; g_errbuf[i++] = c; g_errbuf[i++] = '\'';
    g_errbuf[i] = 0;
}

int         cc_failed(void) { return g_err; }
const char *cc_errmsg(void) { return g_errbuf; }

/* ---------- output sink (UART, or a capture buffer for HTTP) ---------- */

static char *g_cap;            /* capture buffer, or NULL = write to UART */
static int   g_capcap, g_caplen;

static void emit_ch(char c)
{
    if (g_cap) { if (g_caplen < g_capcap - 1) g_cap[g_caplen++] = c; }
    else uart_putc(c);
}
static void emit_str(const char *s) { while (*s) emit_ch(*s++); }
static void emit_dec(long v)
{
    char buf[24]; int n = 0; unsigned long u;
    if (v < 0) { emit_ch('-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (u == 0) { emit_ch('0'); return; }
    while (u > 0) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (n--) emit_ch(buf[n]);
}

/* ---------- runaway-loop guard ---------- */
/* The compiler injects a call to cc_tick() at every loop back-edge.  It
 * returns 1 while budget remains and 0 once exhausted, which makes the
 * generated code break out of the loop — so a `while(1){}` submitted to
 * /compile cannot hard-hang the device. */
#define CC_BUDGET 200000000L
static long g_budget;
static int  g_aborted;

static int cc_tick(void)
{
    if (g_budget <= 0) { g_aborted = 1; return 0; }
    g_budget--;
    return 1;
}

/* ---------- runtime builtins exposed to compiled code ---------- */

static void cc_print(long v)       { emit_dec(v); emit_ch('\n'); }
static void cc_putchar(long c)     { emit_ch((char)c); }
static void cc_puts(const char *s) { emit_str(s); emit_ch('\n'); }

static long cc_actor_send(long id, const char *method, long arg)
{
    int out = 0;
    if (actor_message((int)id, method, (int)arg, &out) != 0) return -1;
    return (long)out;
}

unsigned long cc_resolve_extern(const char *name)
{
    struct { const char *n; void *f; } tab[] = {
        { "print",      (void *)&cc_print     },
        { "putchar",    (void *)&cc_putchar   },
        { "putc",       (void *)&cc_putchar   },
        { "puts",       (void *)&cc_puts      },
        { "actor_send", (void *)&cc_actor_send},
        { "__cc_tick",  (void *)&cc_tick      },
        { 0, 0 }
    };
    for (int i = 0; tab[i].n; i++) {
        const char *x = tab[i].n, *y = name;
        while (*x && *y && *x == *y) { x++; y++; }
        if (*x == 0 && *y == 0) return (unsigned long)tab[i].f;
    }
    return 0;
}

/* ---------- I-cache / D-cache sync for JIT ---------- */

static void cc_sync_icache(void *p, unsigned long len)
{
    unsigned long start = (unsigned long)p;
    unsigned long end   = start + len;
    for (unsigned long a = start; a < end; a += 16)
        __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
    for (unsigned long a = start; a < end; a += 16)
        __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy; isb" ::: "memory");
}

/* ---------- the `cc` shell command ---------- */

#define CC_ARENA   (256 * 1024)
#define CC_CODECAP ( 64 * 1024)

/* Shared core: compile `src` (n bytes) and execute in place.  Program
 * output goes wherever the sink (g_cap / UART) currently points.
 * Returns 0 (sets *retval), -1 on compile error (g_errbuf set), -2 OOM. */
static int compile_run_core(const char *src, unsigned long n, long *retval)
{
    if (!arena_init(CC_ARENA)) return -2;
    g_err = 0; g_errbuf[0] = 0;

    char *s = (char *)cc_alloc(n + 1);
    for (unsigned long i = 0; i < n; i++) s[i] = src[i];
    s[n] = 0;

    token_t *toks = cc_lex(s);
    func_t  *fns  = cc_failed() ? 0 : cc_parse(toks);

    unsigned char *code = 0;
    int entry = 0, len = -1;
    if (!cc_failed()) {
        code = (unsigned char *)kmalloc(CC_CODECAP);
        if (!code) { arena_free(); return -2; }
        len = cc_codegen(fns, code, CC_CODECAP, &entry);
    }
    if (cc_failed() || len < 0) { if (code) kfree(code); arena_free(); return -1; }

    cc_sync_icache(code, (unsigned long)len);
    g_budget = CC_BUDGET; g_aborted = 0;
    long (*entryfn)(void) = (long (*)(void))(code + entry);
    long rc = entryfn();
    if (retval) *retval = rc;

    kfree(code);
    arena_free();
    return 0;
}

int cc_run_source(const char *src, int srclen, char *out, int outcap, long *retval)
{
    g_cap = out; g_capcap = outcap; g_caplen = 0;
    if (out && outcap > 0) out[0] = 0;

    long rv = 0;
    int rc = compile_run_core(src, (unsigned long)(srclen < 0 ? 0 : srclen), &rv);

    if (rc == 0 && g_aborted && g_cap) {
        const char *note = "cc: aborted (runaway loop budget exhausted)\n";
        for (int i = 0; note[i] && g_caplen < g_capcap - 1; i++) g_cap[g_caplen++] = note[i];
    }
    if (g_cap) { g_cap[(g_caplen < g_capcap) ? g_caplen : (g_capcap - 1)] = 0; }
    g_cap = 0;

    if (rc == 0) { if (retval) *retval = rv; return 0; }

    /* compile error / OOM: report into `out` */
    if (out && outcap > 0) {
        int p = 0;
        const char *pfx = "cc: ";
        while (*pfx && p < outcap - 1) out[p++] = *pfx++;
        const char *e = g_errbuf[0] ? g_errbuf : (rc == -2 ? "out of memory" : "compile error");
        while (*e && p < outcap - 1) out[p++] = *e++;
        out[p] = 0;
    }
    return rc;
}

int cmd_cc(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: cc <file.c>\n"); return -1; }

    vfs_node_t *f = vfs_resolve(argv[1]);
    if (!f || f->kind != VFS_FILE) {
        uart_puts("cc: no such file: "); uart_puts(argv[1]); uart_puts("\n");
        return -1;
    }

    static char out[4096];
    long rv = 0;
    int rc = cc_run_source((const char *)f->data, (int)f->size, out, sizeof out, &rv);
    uart_puts(out);
    if (rc == 0) { uart_puts("=> "); emit_dec(rv); uart_puts("\n"); }
    else         { uart_puts("\n"); }
    return rc == 0 ? 0 : -1;
}
