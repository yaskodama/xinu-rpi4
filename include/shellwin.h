// include/shellwin.h — Shell-as-a-window.
//
// A scroll-back console rendered into one wm window.  Every byte
// that flows through uart_putc() is also captured into a fixed
// ring of text lines so the shell stays readable on HDMI even
// after the wm has repainted the desktop.
//
// Input is still UART-only for now (USB HID will land in phase
// M5).  shellwin_step() polls the PL011 RX FIFO non-blockingly,
// builds up a line, and dispatches via shell_dispatch_line() once
// a CR/LF arrives — so the wm_run() frame loop can drive the
// shell without giving up the CPU to a blocking uart_getline().

#ifndef XINU_RPI4_SHELLWIN_H
#define XINU_RPI4_SHELLWIN_H

#include "wm.h"

/* Scrollback ring size in the shell window.  40 rows × 72 cols: a tall
 * on-demand shell window (see shellwin_new) shows up to 40 rows under our
 * 640×480 layout with the 8×8 font; the shorter primary Shell 1 just uses
 * fewer of them.  Each row stores a NUL-terminated string. */
#define SHELLWIN_ROWS    40
#define SHELLWIN_COLS    72

/* Number of on-demand shell windows that can be created beyond the always-on
 * primary Shell 1 — so the desktop tops out at 1 + SHELLWIN_EXTRA = 3 shells. */
#define SHELLWIN_EXTRA   2

/* Clear the ring and reset write head.  Must be called before any
 * uart traffic if shellwin output is to be captured. */
void shellwin_init(void);

/* Append one character to the ring.  '\n' advances to the next
 * row (carriage return is ignored — uart_puts() translates).
 * Backspace / DEL erase the previous column on the current row.
 * Other control chars are dropped.  Safe before shellwin_init()
 * (no-op) so wiring it into uart_putc() can't crash early boot. */
void shellwin_record_char(char c);

/* Clear the scrollback of the current output shell (backs the `clear` cmd). */
void shellwin_clear_current(void);

/* draw_content callback for wm — paints the visible ring onto
 * self's content area in chronological order. */
void shellwin_draw(window_t *self, unsigned int frame);

/* Drive one non-blocking shell step.  Drains the UART RX FIFO,
 * accumulates an input line, dispatches via shell_dispatch_line()
 * on CR/LF.  Designed to be called from the wm_run() frame loop. */
void shellwin_step(void);

/* Feed one keypress into the shell input buffer.  Same line-editor
 * behaviour as the UART path (CR/LF → dispatch, BS/DEL → backspace,
 * printable → echo + buffer).  Used by the USPi keyboard handler
 * so USB keystrokes share the UART input path. */
void shellwin_handle_key(char c);

/* The window descriptor itself — laid out / wm_add()'d by
 * loader/main.c.  draw_content already points at shellwin_draw. */
extern window_t shell_win;

/* On-demand shells — each a fully independent console (own ring/input/history;
 * its commands run in a dedicated process).  Created/raised by the right-click
 * context menu via shellwin_new() (up to SHELLWIN_EXTRA of them). */
extern window_t shell_x[SHELLWIN_EXTRA];
void shellwin_new(void);

/* If `aw` is one of the on-demand shell windows, feed it the keypress and
 * return 1; otherwise return 0 so the caller can route elsewhere. */
int shellwin_route_key(window_t *aw, char c);

#endif /* XINU_RPI4_SHELLWIN_H */
