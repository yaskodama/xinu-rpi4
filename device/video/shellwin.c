// device/video/shellwin.c — wm-managed shell console.
//
// The kernel previously had two mutually exclusive interaction
// modes: with HDMI plugged in, wm_run() captured the frame loop
// forever and the UART REPL was unreachable; without HDMI it
// fell back to shell_main() on the UART.  This window unifies
// them: every uart_putc() is also captured into a ring of text
// lines, and the wm frame loop polls UART input non-blockingly
// so the shell stays responsive while the rest of the windows
// keep animating.

#include "shellwin.h"
#include "video.h"
#include "uart.h"
#include "shell.h"

window_t shell_win;

/* Scrollback ring: row indices wrap modulo SHELLWIN_ROWS, and
 * `ring_filled` flips once we've written past the bottom and
 * are now overwriting the oldest line. */
static char ring[SHELLWIN_ROWS][SHELLWIN_COLS + 1];
static int  cur_row;
static int  cur_col;
static int  ring_filled;
static int  inited;

/* Pending input line (UART → shell_dispatch_line).  Mirrors the
 * line-editor in uart_getline but stays non-blocking. */
static char inbuf[SHELL_BUFLEN];
static int  inlen;

/* Single-entry history: the previous dispatched line.  Up-arrow recalls
 * it (most common use of the arrow keys in a simple shell).  We keep it
 * intentionally small — full history is a separate feature. */
static char histbuf[SHELL_BUFLEN];
static int  histlen;

/* ANSI escape state.  HID arrow keys come as ESC [ A/B/C/D etc., one
 * byte per shellwin_handle_key() call (the network /type route sends
 * them as a 3-byte sequence likewise).  States:
 *   0 = normal
 *   1 = saw ESC (waiting for '[' or 'O' or final char)
 *   2 = saw ESC '['
 *   3 = saw ESC 'O' (for F1-F4) */
static int  esc_state;

static void newline(void)
{
    ring[cur_row][cur_col] = 0;
    cur_row = (cur_row + 1) % SHELLWIN_ROWS;
    cur_col = 0;
    ring[cur_row][0] = 0;
    if (cur_row == 0) ring_filled = 1;
}

void shellwin_init(void)
{
    for (int r = 0; r < SHELLWIN_ROWS; r++) ring[r][0] = 0;
    cur_row = 0;
    cur_col = 0;
    ring_filled = 0;
    inlen = 0;
    histlen = 0;
    esc_state = 0;
    inited = 1;
}

void shellwin_record_char(char c)
{
    if (!inited) return;
    if (c == '\r') return;        /* uart_puts translates \n → \r\n */
    if (c == '\n') { newline(); return; }
    if (c == 0x08 || c == 0x7F) { /* BS / DEL */
        if (cur_col > 0) {
            cur_col--;
            ring[cur_row][cur_col] = 0;
        }
        return;
    }
    if (c < 0x20) return;          /* drop other control chars */

    if (cur_col >= SHELLWIN_COLS) newline();
    ring[cur_row][cur_col++] = c;
    ring[cur_row][cur_col] = 0;
}

void shellwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;

    int fs = self->font_scale > 0 ? self->font_scale : 1;
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + 4;
    const int line_h = (FONT_HEIGHT + 1) * fs;

    /* Cap visible rows to what physically fits inside the window's
     * content area — the ring may carry more lines than the window
     * can show.  Always display the newest tail of the ring. */
    int content_h = self->height - WM_TITLEBAR_H - 7;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHELLWIN_ROWS) max_rows = SHELLWIN_ROWS;

    int have = ring_filled ? SHELLWIN_ROWS : cur_row + 1;
    int rows = have < max_rows ? have : max_rows;

    /* `start` = oldest row to display.  We want rows ending at cur_row,
     * walking backwards `rows-1` steps modulo SHELLWIN_ROWS. */
    int start = (cur_row - rows + 1 + SHELLWIN_ROWS) % SHELLWIN_ROWS;

    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHELLWIN_ROWS;
        draw_string_scaled(cx, cy + i * line_h,
                           ring[r], 0xFFCCE0FFU, self->content_bg, fs);
    }
}

/* Backspace the entire input line off the display (used by Up-arrow
 * before replaying history, and by Ctrl-U to cancel the current line). */
static void erase_input(void)
{
    while (inlen > 0) {
        inlen--;
        uart_putc('\b'); uart_putc(' '); uart_putc('\b');
    }
}

/* Replay a buffer to inbuf + screen.  Used by Up-arrow (history recall)
 * and Ctrl-U-then-redraw style operations. */
static void replay_input(const char *buf, int len)
{
    int n = len < (int)sizeof(inbuf) - 1 ? len : (int)sizeof(inbuf) - 1;
    for (int i = 0; i < n; i++) {
        inbuf[inlen++] = buf[i];
        uart_putc(buf[i]);
    }
}

void shellwin_handle_key(char c)
{
    if (!inited) return;

    /* --- ANSI escape sequence parser ---------------------------------
     * ESC '[' X  → arrow keys / Home / End
     * ESC 'O' X  → F1..F4
     * Anything unexpected aborts the sequence (state back to 0). */
    if (esc_state == 1) {
        if (c == '[')      { esc_state = 2; return; }
        else if (c == 'O') { esc_state = 3; return; }
        else               { esc_state = 0; /* drop the lone ESC */ return; }
    }
    if (esc_state == 2) {
        esc_state = 0;
        if (c == 'A') {            /* Up — recall history */
            erase_input();
            replay_input(histbuf, histlen);
        } else if (c == 'B') {     /* Down — clear input */
            erase_input();
        }
        /* C (right), D (left), H (home), F (end), '~'-terminated multi-char
         * sequences (PageUp/Down, F5+) — silently ignored for now; full
         * line editing would need a separate input-line redraw path. */
        return;
    }
    if (esc_state == 3) {
        esc_state = 0;
        /* F1..F4 (P/Q/R/S) — currently no binding; ignore. */
        return;
    }

    if (c == 0x1b) {                /* ESC — start sequence */
        esc_state = 1;
        return;
    }

    if (c == 0x15) {                /* Ctrl-U — clear current line */
        erase_input();
        return;
    }
    if (c == 0x03) {                /* Ctrl-C — abort line, fresh prompt */
        erase_input();
        uart_puts("\nxinu-pi4$ ");
        return;
    }

    if (c == '\r' || c == '\n') {
        uart_putc('\n');
        if (inlen > 0) {
            inbuf[inlen] = 0;
            /* Save to history BEFORE dispatch (dispatch may clobber inbuf
             * if a command does its own keyboard reads in future). */
            int n = inlen < (int)sizeof(histbuf) ? inlen : (int)sizeof(histbuf) - 1;
            for (int i = 0; i < n; i++) histbuf[i] = inbuf[i];
            histlen = n;
            shell_dispatch_line(inbuf);
            inlen = 0;
        }
        uart_puts("xinu-pi4$ ");
    } else if (c == 0x08 || c == 0x7F) {
        if (inlen > 0) {
            inlen--;
            uart_putc('\b'); uart_putc(' '); uart_putc('\b');
        }
    } else if (c >= 0x20 && c < 0x7F) {
        if (inlen < (int)sizeof(inbuf) - 1) {
            inbuf[inlen++] = c;
            uart_putc(c);
        }
    }
    /* Other control chars (Tab, F-keys, etc.) — dropped silently. */
}

void shellwin_step(void)
{
    if (!inited) return;

    /* Drain the UART RX FIFO and feed shellwin_handle_key() — this IS the
     * serial shell: shellwin_handle_key() prints the xinu-pi4$ prompt, echoes
     * input, and dispatches commands, all to the UART.  (shell_main() in
     * shell.c is never reached — wm_run() above it never returns.)  USB
     * keyboard input enters via the same shellwin_handle_key() path. */
    int c;
    while ((c = uart_poll_char()) >= 0) {
        shellwin_handle_key((char)c);
    }
}
