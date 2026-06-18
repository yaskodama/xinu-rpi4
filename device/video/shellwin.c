// device/video/shellwin.c — wm-managed shell consoles.
//
// Shell 1 (shell_win) is the primary console: every uart_putc() byte (boot log,
// command output, serial echo) is captured into its scrollback ring, and the wm
// frame loop polls UART RX so the serial REPL stays live.
//
// Up to SHELLWIN_EXTRA additional shells (shell_x[]) are FULLY INDEPENDENT —
// each has its own scrollback ring, input line, history, prompt, and Xinu
// worker process — created on demand from the right-click "New Shell window"
// menu via shellwin_new().  The desktop tops out at 1 + SHELLWIN_EXTRA shells.

#include "shellwin.h"
#include "video.h"
#include "uart.h"
#include "shell.h"
#include "proc.h"

/* ---- one terminal's state (scrollback ring + line editor) ---- */
struct shellterm {
    char ring[SHELLWIN_ROWS][SHELLWIN_COLS + 1];
    int  cur_row, cur_col, ring_filled;
    char inbuf[SHELL_BUFLEN];
    int  inlen;
    char histbuf[SHELL_BUFLEN];     /* single-entry history (Up-arrow recall) */
    int  histlen;
    int  esc_state;                 /* ANSI ESC parser: 0 / 1=ESC / 2=ESC[ / 3=ESCO */
};

static struct shellterm term1;                    /* primary Shell 1 (independent) */
static struct shellterm term_x[SHELLWIN_EXTRA];   /* on-demand independent shells */
/* Output-capture target.  NULL = "nobody" — ambient uart_putc output (boot log,
 * net/diagnostic traces, the serial line in general) then goes only to the
 * physical UART + HDMI console and is NOT mirrored into any shell window.  A
 * shell sets this to its own term *only* while dispatching its own command, so
 * each window shows just its command output — the serial line and Shell 1 are
 * fully separate I/O channels. */
static struct shellterm *g_out = 0;
static int inited;

window_t shell_win;                  /* primary Shell 1 */
window_t shell_x[SHELLWIN_EXTRA];    /* on-demand Shell 2.. */

/* ---- low-level: write into a specific terminal's ring ---- */
static void term_newline(struct shellterm *t)
{
    t->ring[t->cur_row][t->cur_col] = 0;
    t->cur_row = (t->cur_row + 1) % SHELLWIN_ROWS;
    t->cur_col = 0;
    t->ring[t->cur_row][0] = 0;
    if (t->cur_row == 0) t->ring_filled = 1;
}
static void term_putc(struct shellterm *t, char c)
{
    if (c == '\r') return;
    if (c == '\n') { term_newline(t); return; }
    if (c == 0x08 || c == 0x7F) {
        if (t->cur_col > 0) { t->cur_col--; t->ring[t->cur_row][t->cur_col] = 0; }
        return;
    }
    if (c < 0x20) return;
    if (t->cur_col >= SHELLWIN_COLS) term_newline(t);
    t->ring[t->cur_row][t->cur_col++] = c;
    t->ring[t->cur_row][t->cur_col] = 0;
}
static void term_puts(struct shellterm *t, const char *s) { while (*s) term_putc(t, *s++); }

/* uart_putc() tee — capture serial / boot / command output into the CURRENT
 * target (term1 normally; an extra shell's term while its worker runs).  Safe
 * before shellwin_init(). */
void shellwin_record_char(char c)
{
    if (!inited || !g_out) return;     /* ambient/serial output isn't a shell's */
    term_putc(g_out, c);
}

/* `clear` shell command — wipe the scrollback ring of whichever shell is the
 * current output target (Shell 1 when a command runs inline; an extra shell's
 * term while its worker runs).  Leaves the input line and history intact. */
void shellwin_clear_current(void)
{
    if (!inited || !g_out) return;
    struct shellterm *t = g_out;
    for (int r = 0; r < SHELLWIN_ROWS; r++) t->ring[r][0] = 0;
    t->cur_row = t->cur_col = t->ring_filled = 0;
}

static void term_reset(struct shellterm *t)
{
    for (int r = 0; r < SHELLWIN_ROWS; r++) t->ring[r][0] = 0;
    t->cur_row = t->cur_col = t->ring_filled = t->inlen = t->histlen = t->esc_state = 0;
}

void shellwin_init(void)
{
    term_reset(&term1);
    for (int i = 0; i < SHELLWIN_EXTRA; i++) term_reset(&term_x[i]);
    g_out = 0;                 /* ambient output captured by nobody until a cmd runs */
    inited = 1;
}

/* ---- drawing (maps the window to its terminal) ---- */
static int extra_index_of_win(window_t *self)
{
    for (int i = 0; i < SHELLWIN_EXTRA; i++) if (self == &shell_x[i]) return i;
    return -1;
}
static struct shellterm *term_of(window_t *self)
{
    int i = extra_index_of_win(self);
    return (i >= 0) ? &term_x[i] : &term1;
}

#define SH_PROMPT      "xinu-pi4$ "
#define SH_PROMPT_LEN  10

void shellwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    struct shellterm *t = term_of(self);
    int fs = self->font_scale > 0 ? self->font_scale : 1;
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + 4;
    const int line_h = (FONT_HEIGHT + 1) * fs;

    int content_h = self->height - WM_TITLEBAR_H - 7;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHELLWIN_ROWS) max_rows = SHELLWIN_ROWS;

    /* Reserve the bottom line for the LIVE input (prompt + edit buffer); the
     * scrollback ring fills the rows above it.  The line being typed is NOT
     * stored in the ring, so async output teed into the ring while you type
     * (boot log, command output, the per-packet "net: ..." traces from the
     * ICMP/ARP responder) can never split or scramble your input — the old
     * behaviour, and the cause of Shell 1's "weird prompt". */
    int ring_rows = max_rows - 1;
    if (ring_rows < 0) ring_rows = 0;

    /* The ring's current write row (cur_row) is where the NEXT output byte will
     * land.  When it's still empty (cur_col == 0 — e.g. right after a command
     * whose output ended in '\n'), it is NOT a real scrollback line: the live
     * input line takes its place.  Drawing it as a blank row AND the input line
     * below it left a spurious empty line above the new prompt.  Only when the
     * current row holds partial output (cur_col > 0) is it shown, with the
     * input line beneath it. */
    int empty_cur = (t->cur_col == 0);
    int tail = empty_cur ? (t->cur_row - 1 + SHELLWIN_ROWS) % SHELLWIN_ROWS
                         : t->cur_row;
    int have = t->ring_filled ? (SHELLWIN_ROWS - (empty_cur ? 1 : 0))
                              : (t->cur_row + (empty_cur ? 0 : 1));
    if (have < 0) have = 0;
    int rows = have < ring_rows ? have : ring_rows;
    int start = (tail - rows + 1 + SHELLWIN_ROWS) % SHELLWIN_ROWS;
    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHELLWIN_ROWS;
        draw_string_scaled(cx, cy + i * line_h, t->ring[r], 0xFFCCE0FFU, self->content_bg, fs);
    }

    /* Live input line: prompt + the bytes typed so far, placed RIGHT AFTER the
     * last scrollback row (like a real terminal) — not pinned to the window
     * bottom, which left a gap above the prompt when the ring wasn't full. */
    int iy = cy + rows * line_h;
    char line[SH_PROMPT_LEN + SHELL_BUFLEN + 1];
    int p = 0;
    for (const char *q = SH_PROMPT; *q; q++) line[p++] = *q;
    for (int i = 0; i < t->inlen && p < (int)sizeof(line) - 1; i++) line[p++] = t->inbuf[i];
    line[p] = 0;
    draw_string_scaled(cx, iy, line, 0xFFCCE0FFU, self->content_bg, fs);

    /* Cursor: underline just past the last typed column.  Bright white when
     * this shell has keyboard focus, dim otherwise. */
    extern window_t *wm_active(void);
    int col = (p < SHELLWIN_COLS) ? p : SHELLWIN_COLS - 1;
    int curx = cx + col * FONT_WIDTH * fs;
    unsigned int ccol = (wm_active() == self) ? 0xFFFFFFFFU : 0xFF5A7A5AU;
    fill_rect(curx, iy + (FONT_HEIGHT - 2) * fs, FONT_WIDTH * fs, 2 * fs, ccol);
}

/* ---- per-extra-shell worker process: runs that shell's commands independently
 * of Shell 1 and of every other extra shell.  Each worker recovers its index
 * from proctab[currpid].arg (stashed at proc_create_arg time). ---- */
static volatile int  shx_pid[SHELLWIN_EXTRA];
static volatile int  shx_busy[SHELLWIN_EXTRA];    /* a command is queued/running */
static char          shx_cmd[SHELLWIN_EXTRA][SHELL_BUFLEN];

static void shell_extra_proc(void)
{
    int idx = (int)(long)proctab[currpid].arg;
    for (;;) {
        while (!shx_busy[idx]) proc_block();         /* wait for the next command */
        struct shellterm *save = g_out;
        g_out = &term_x[idx];                        /* capture output into this shell */
        shell_dispatch_line(shx_cmd[idx]);
        g_out = save;
        shx_busy[idx] = 0;                           /* prompt is the live input line */
    }
}

/* Hand a typed line to extra shell `idx`'s worker (or run it inline if the
 * process isn't up yet).  Drops a new command while one is still running. */
static void extra_submit(int idx, const char *line)
{
    if (shx_pid[idx] < 0) {                          /* no process — inline fallback */
        struct shellterm *save = g_out; g_out = &term_x[idx];
        shell_dispatch_line((char *)line);
        g_out = save;
        return;
    }
    if (shx_busy[idx]) return;
    int i = 0; for (; line[i] && i < SHELL_BUFLEN - 1; i++) shx_cmd[idx][i] = line[i];
    shx_cmd[idx][i] = 0;
    shx_busy[idx] = 1;
    proc_ready(shx_pid[idx]);
}

/* ---- shared line editor for either terminal ---- */
static int extra_index_of_term(struct shellterm *t)
{
    for (int i = 0; i < SHELLWIN_EXTRA; i++) if (t == &term_x[i]) return i;
    return -1;
}

/* Commit the line currently in the edit buffer into the scrollback ring as
 * "xinu-pi4$ <text>", so executed commands stay visible above the live input
 * line.  Echo never touches the ring while typing — only this commit does. */
static void term_commit_line(struct shellterm *t, const char *suffix)
{
    term_puts(t, SH_PROMPT);
    for (int i = 0; i < t->inlen; i++) term_putc(t, t->inbuf[i]);
    if (suffix) term_puts(t, suffix);
    term_newline(t);
}

static void term_handle_key(struct shellterm *t, char c)
{
    if (!inited) return;

    if (t->esc_state == 1) {
        if (c == '[')      { t->esc_state = 2; return; }
        else if (c == 'O') { t->esc_state = 3; return; }
        else               { t->esc_state = 0; return; }
    }
    if (t->esc_state == 2) {
        t->esc_state = 0;
        if (c == 'A') {                                  /* Up — recall history  */
            int n = t->histlen < (int)sizeof(t->inbuf) - 1 ? t->histlen
                                                           : (int)sizeof(t->inbuf) - 1;
            for (int i = 0; i < n; i++) t->inbuf[i] = t->histbuf[i];
            t->inlen = n;
        } else if (c == 'B') { t->inlen = 0; }           /* Down — clear line     */
        return;
    }
    if (t->esc_state == 3) { t->esc_state = 0; return; }

    if (c == 0x1b) { t->esc_state = 1; return; }            /* ESC */
    if (c == 0x15) { t->inlen = 0; return; }                /* Ctrl-U — clear line */
    if (c == 0x03) { term_commit_line(t, "^C"); t->inlen = 0; return; }  /* Ctrl-C */

    if (c == '\r' || c == '\n') {
        int xi = extra_index_of_term(t);
        if (t->inlen > 0) {
            t->inbuf[t->inlen] = 0;
            int n = t->inlen < (int)sizeof(t->histbuf) ? t->inlen : (int)sizeof(t->histbuf) - 1;
            for (int i = 0; i < n; i++) t->histbuf[i] = t->inbuf[i];
            t->histlen = n;
        }
        term_commit_line(t, 0);                             /* echo cmd to scrollback */
        if (t->inlen > 0) {
            if (xi >= 0) {
                extra_submit(xi, t->inbuf);                 /* runs in its worker  */
            } else {
                /* Shell 1 runs inline (wm thread).  Capture its output into
                 * term1 for the duration only, so the serial line / ambient
                 * output stays out of this window. */
                struct shellterm *save = g_out;
                g_out = &term1;
                shell_dispatch_line(t->inbuf);
                g_out = save;
            }
        }
        t->inlen = 0;
    } else if (c == 0x08 || c == 0x7F) {                    /* Backspace / DEL */
        if (t->inlen > 0) t->inlen--;
    } else if (c >= 0x20 && c < 0x7F) {                     /* printable */
        if (t->inlen < (int)sizeof(t->inbuf) - 1) t->inbuf[t->inlen++] = c;
    }
}

void shellwin_handle_key(char c) { term_handle_key(&term1, c); }   /* Shell 1 */

/* Route a keypress to an on-demand shell window if `aw` is one of them. */
int shellwin_route_key(window_t *aw, char c)
{
    int i = extra_index_of_win(aw);
    if (i < 0) return 0;
    term_handle_key(&term_x[i], c);
    return 1;
}

void shellwin_step(void)
{
    if (!inited) return;
    /* Serial line is a SEPARATE I/O channel from the on-screen shells: drain
     * the UART RX FIFO so it can't overflow, but do NOT feed those bytes to
     * Shell 1.  Shell 1 takes input only from the keyboard / network keystroke
     * route.  (A floating/unwired Pi-4 RX otherwise injected phantom bytes that
     * the shell ran as garbage commands — the "command-not-found" crosstalk.
     * A dedicated serial REPL is shell_main(), used only in the no-HDMI path.) */
    while (uart_poll_char() >= 0) { /* discard serial RX */ }
}

/* Create (or, once all are built, raise) an on-demand shell window — bound by
 * the right-click context menu's "New Shell window".  Each new shell is laid
 * out 40 text rows tall, gets its own scrollback/input/history, and spawns its
 * own worker process. */
static int extra_count;   /* how many extra shells have been built (0..SHELLWIN_EXTRA) */

void shellwin_new(void)
{
    extern void wm_show(window_t *);

    if (extra_count >= SHELLWIN_EXTRA) {       /* all built — just raise the newest */
        wm_show(&shell_x[SHELLWIN_EXTRA - 1]);
        return;
    }

    int i = extra_count;
    window_t *w = &shell_x[i];

    /* 40 visible rows: line_h = (FONT_HEIGHT+1) = 9 px at scale 1, plus the
     * title bar (WM_TITLEBAR_H) and the draw fn's 7 px of top/bottom padding.
     * 40*9 + 12 + 7 = 379 -> 380 (max_rows = (380-19)/9 = 40).  Cascade the
     * windows so a second one doesn't land exactly on the first. */
    w->x = 60 + i * 30;
    w->y = 36 + i * 30;
    w->width  = 500;
    w->height = 380;

    char nm[8] = "Shell ?";                    /* "Shell 2" / "Shell 3" */
    nm[6] = (char)('2' + i);
    int k; for (k = 0; k < WM_TITLE_MAX && nm[k]; k++) w->title[k] = nm[k];
    w->title[k] = 0;

    w->font_scale   = 1;
    w->chrome_color = 0xFF80E080U;
    w->title_bg     = 0xFF205020U;
    w->title_fg     = 0xFFFFFFFFU;
    w->content_bg   = 0xFF0A140AU;
    w->draw_content = shellwin_draw;

    char pn[8] = "shell?";                      /* unique process name */
    pn[5] = (char)('2' + i);
    shx_pid[i] = proc_create_arg(shell_extra_proc, 16384, pn, (void *)(long)i);

    term_puts(&term_x[i], "Shell ");
    { char b[2] = { (char)('2' + i), 0 }; term_puts(&term_x[i], b); }
    term_puts(&term_x[i], " (independent process)\n");

    extra_count++;
    wm_show(w);
}
