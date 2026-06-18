// device/video/basicwin.c — wm-managed BASIC window as a full-screen text
// editor.
//
// No prompt — just a blinking-free block cursor in a large space-padded text
// grid (~10 screens of scrollback).  Arrow keys move the cursor freely; typing
// inserts at the cursor; Enter runs the cursor's line through basic_exec_line()
// and the interpreter's output (bw_emit) flows from the cursor like a terminal.
// The view auto-scrolls to keep the cursor visible; PageUp/PageDown and the
// up/down arrows at the edges scroll the 10-screen buffer.

#include "basicwin.h"
#include "video.h"

/* ---- interpreter seams (device/video/basic.c) ---- */
extern void basic_init(void);
extern void basic_exec_line(const char *line);
extern void basic_set_emit(void (*fn)(const char *));
extern void basic_set_cls(void (*fn)(int));
extern void basic_set_pause(void (*fn)(int));
extern void basic_set_break_poll(int (*fn)(void));
extern void basic_set_line(void (*fn)(int, int, int, int, int));
extern void basic_set_circle(void (*fn)(int, int, int, int));
extern void basic_set_plot(void (*fn)(int, int, int));
extern void basic_set_gfx_active(int (*fn)(void));
extern void basic_break(void);             /* request a RUN-loop break */
extern int  xhci_poll_ctrl_c(void);        /* polls "is Ctrl+C held?" */
extern void wm_cursor_after_blit(void);    /* pump mouse + re-stamp cursor */

#define BW_COLS   96            /* chars per row (fixed grid)              */
#define BW_ROWS   360           /* ~10 screens of scrollback              */

/* ---- toolbar buttons --------------------------------------------------
 * A clickable strip just below the title bar.  Each button injects a BASIC
 * direct command (FILES / LIST / RUN "<sample>") — the samples are embedded
 * in basic.c, so no filesystem is needed. */
typedef struct { const char *label; const char *cmd; } bw_button_t;
static const bw_button_t bw_btns[] = {
    { "FILES", "FILES"          },
    { "LIST",  "LIST"           },
    { "hanoi", "RUN \"hanoi\""  },
    { "bsort", "RUN \"bsort\""  },
    { "fizz",  "RUN \"fizz\""   },
    { "qsort", "RUN \"qsort\""  },
    { "koch",  "RUN \"koch\""   },
};
#define BW_NBTN      ((int)(sizeof(bw_btns) / sizeof(bw_btns[0])))
#define BW_BTN_H     16
#define BW_BTN_GAP   2
#define BW_TOOLBAR_H (BW_BTN_H + 4)     /* button strip height below titlebar */

static int bw_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Button i rectangle in WINDOW-LOCAL coords (relative to self->x/self->y). */
static void bw_btn_rect(int i, int win_w, int *bx, int *by, int *bw, int *bh)
{
    int avail = win_w - 4 - BW_BTN_GAP * (BW_NBTN - 1);
    int w = avail / BW_NBTN;
    if (w < 1) w = 1;
    *bw = w; *bh = BW_BTN_H;
    *bx = 2 + i * (w + BW_BTN_GAP);
    *by = WM_TITLEBAR_H + 2;
}

window_t basic_win;

/* Space-padded grid: every row is BW_COLS chars + a trailing NUL for drawing. */
static char grid[BW_ROWS][BW_COLS + 1];
static int  cur_row, cur_col;   /* cursor in buffer coords                 */
static int  view_top;           /* first buffer row shown                  */
static int  inited;
static int  esc_state;          /* ANSI escape parser: 0 none,1 ESC,2 ESC[ */
static int  gfx_on;             /* 1 once a program enters graphics mode    */

/* Graphics canvas rect = the content area below the toolbar.  BASIC's
 * LINE/CIRCLE/PLOT are drawn here via the shared gfx_* display list. */
static void bw_gfx_rect(window_t *self, int *gx, int *gy, int *gw, int *gh)
{
    *gx = self->x + 1;
    *gy = self->y + WM_TITLEBAR_H + BW_TOOLBAR_H + 2;
    *gw = self->width - 2;
    *gh = self->height - WM_TITLEBAR_H - BW_TOOLBAR_H - 3;
}

static void clear_row(int r) { for (int i = 0; i < BW_COLS; i++) grid[r][i] = ' '; grid[r][BW_COLS] = 0; }
static void clear_all(void)  { for (int r = 0; r < BW_ROWS; r++) clear_row(r); cur_row = cur_col = view_top = 0; }

/* Scroll the whole buffer up by one row (oldest line lost) and keep the cursor
 * on the last row.  Used when output/typing runs off the bottom. */
static void scroll_buffer(void)
{
    for (int r = 0; r < BW_ROWS - 1; r++)
        for (int i = 0; i <= BW_COLS; i++) grid[r][i] = grid[r + 1][i];
    clear_row(BW_ROWS - 1);
    if (cur_row > 0) cur_row--;
    if (view_top > 0) view_top--;
}

static void cursor_down_one(void)      /* advance to next row, scrolling if needed */
{
    cur_row++;
    if (cur_row >= BW_ROWS) { scroll_buffer(); cur_row = BW_ROWS - 1; }
}

/* ---- interpreter output sink: terminal-style write at the cursor ---- */
static void bw_putc(char c)
{
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\n') { cur_col = 0; cursor_down_one(); return; }
    if (c == '\b') { if (cur_col > 0) cur_col--; return; }
    if (c < 0x20 || c > 0x7e) return;
    if (cur_col >= BW_COLS) { cur_col = 0; cursor_down_one(); }
    grid[cur_row][cur_col++] = c;
}
static void bw_emit(const char *s) { while (*s) bw_putc(*s++); }

/* CLS:  bit 1 = text grid, bit 2 = graphics.  CLS=1 text, CLS 2 gfx, CLS 3 both.
 * Any graphics clear turns the window into graphics mode. */
static void bw_cls(int mode)
{
    if (mode & 1) clear_all();
    if (mode & 2) { bgfx_clear(); gfx_on = 1; }
    if (mode == 0) clear_all();          /* bare CLS == text clear */
}

/* Repaint the BASIC graphics area cleanly and flip it to screen — lets the
 * single-threaded interpreter show animation between PAUSEs (the wm render loop
 * is blocked while a program RUNs). */
static void bw_present_gfx(void)
{
    int gx, gy, gw, gh;
    bw_gfx_rect(&basic_win, &gx, &gy, &gw, &gh);
    fill_rect(gx, gy, gw, gh, basic_win.content_bg);
    bgfx_render(gx, gy, gw, gh);
    video_present();
    wm_cursor_after_blit();   /* flip wiped the cursor — pump mouse + re-stamp */
}

static void bw_pause(int ms)
{
    extern void wm_cursor_delay_ms(int);
    if (gfx_on) bw_present_gfx();
    /* PAUSE/WAIT is the natural place to notice a Ctrl-C in a tight graphics
     * loop (guard-based polling is too coarse for WAIT loops). */
    if (xhci_poll_ctrl_c()) basic_break();
    /* Pump the cursor while waiting — a plain delay_ms() starves the HID
     * interrupt EP and freezes the pointer for the whole WAIT (e.g. koch). */
    if (ms > 0) wm_cursor_delay_ms(ms);
}

/* ---- graphics seams: BASIC LINE/CIRCLE/PLOT -> shared gfx_* display list.
 * Coords are window-local pixels; gfx_render() offsets them into the content
 * rect each frame.  Colours are BASIC palette indices (gfx uses idx & 7). */
static void bw_line(int x1, int y1, int x2, int y2, int color)
{
    extern void wm_cursor_tick(void);
    static int seg;
    gfx_on = 1; bgfx_line(x1, y1, x2, y2, color);
    /* A deep redraw (e.g. koch level 10 ~ 4^10 segments) runs with no WAIT, so
     * pump the cursor every N segments to keep the pointer alive there too. */
    if ((++seg & 63) == 0) wm_cursor_tick();
}
static void bw_circle(int cx, int cy, int r, int color)
{ gfx_on = 1; bgfx_circle(cx, cy, r, color); }
static void bw_plot(int x, int y, int ch)
{ (void)ch; gfx_on = 1; bgfx_line(x, y, x, y, 7); }   /* 1px dot */
static int  bw_gfx_active(void) { return gfx_on; }

/* Toolbar click: find the button under the local cursor, echo its command,
 * then run it through the interpreter (output flows from the cursor). */
static void bw_on_click(window_t *self, int lx, int ly)
{
    if (!inited) return;
    for (int i = 0; i < BW_NBTN; i++) {
        int bx, by, bw, bh;
        bw_btn_rect(i, self->width, &bx, &by, &bw, &bh);
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
            bw_emit(bw_btns[i].cmd);
            bw_putc('\n');
            basic_exec_line(bw_btns[i].cmd);
            return;
        }
    }
}

void basicwin_init(void)
{
    clear_all();
    esc_state = 0;
    inited = 1;
    basic_init();
    basic_set_emit(bw_emit);
    basic_set_cls(bw_cls);
    basic_set_pause(bw_pause);
    basic_set_break_poll(xhci_poll_ctrl_c); /* Ctrl-C breaks a running program */
    basic_set_line(bw_line);                /* LINE   -> gfx canvas */
    basic_set_circle(bw_circle);            /* CIRCLE -> gfx canvas */
    basic_set_plot(bw_plot);                /* PLOT   -> gfx canvas */
    basic_set_gfx_active(bw_gfx_active);    /* gfx-mode flag (suppresses "Ok") */
    basic_win.on_click = bw_on_click;       /* wire the toolbar */
    /* No prompt — leave the cursor at the top-left of an empty page. */
}

/* ---- drawing ---- */
static int visible_rows(window_t *self, int fs)
{
    int content_h = self->height - WM_TITLEBAR_H - BW_TOOLBAR_H - 7;
    int vr = content_h / ((FONT_HEIGHT + 1) * fs);
    if (vr < 1) vr = 1;
    if (vr > BW_ROWS) vr = BW_ROWS;
    return vr;
}

/* Paint the toolbar button strip just below the title bar. */
static void bw_draw_toolbar(window_t *self)
{
    for (int i = 0; i < BW_NBTN; i++) {
        int bx, by, bw, bh;
        bw_btn_rect(i, self->width, &bx, &by, &bw, &bh);
        int ax = self->x + bx, ay = self->y + by;
        unsigned int bg = (i < 2) ? 0xFF2E5E8AU    /* FILES/LIST = blue  */
                                  : 0xFF1F6E2EU;   /* run "..."  = green */
        fill_rect(ax, ay, bw, bh, bg);
        int lw = bw_strlen(bw_btns[i].label) * FONT_WIDTH;
        int tx = ax + (bw - lw) / 2; if (tx < ax + 1) tx = ax + 1;
        int ty = ay + (bh - FONT_HEIGHT) / 2;
        draw_string_scaled(tx, ty, bw_btns[i].label, 0xFFFFFFFFU, bg, 1);
    }
}

void basicwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    bw_draw_toolbar(self);
    int fs = self->font_scale > 0 ? self->font_scale : 1;
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + BW_TOOLBAR_H + 4;
    const int line_h = (FONT_HEIGHT + 1) * fs;
    int vr = visible_rows(self, fs);

    /* keep the cursor inside the visible window */
    if (cur_row < view_top) view_top = cur_row;
    if (cur_row >= view_top + vr) view_top = cur_row - vr + 1;
    if (view_top < 0) view_top = 0;

    for (int i = 0; i < vr; i++) {
        int r = view_top + i;
        if (r >= BW_ROWS) break;
        draw_string_scaled(cx, cy + i * line_h, grid[r], 0xFFB6FFB6U, self->content_bg, fs);
    }

    /* block cursor at (cur_row,cur_col) if on screen */
    int crow = cur_row - view_top;
    if (crow >= 0 && crow < vr && cur_col < BW_COLS) {
        int px = cx + cur_col * FONT_WIDTH * fs;
        int py = cy + crow * line_h;
        fill_rect(px, py + (FONT_HEIGHT - 2) * fs, FONT_WIDTH * fs, 2 * fs, 0xFFFFFFFFU);
    }

    /* graphics overlay: replay the LINE/CIRCLE/PLOT display list */
    if (gfx_on) {
        int gx, gy, gw, gh;
        bw_gfx_rect(self, &gx, &gy, &gw, &gh);
        bgfx_render(gx, gy, gw, gh);
    }
}

/* ---- editing ---- */
static void insert_char(char c)
{
    /* shift the rest of the row right by one (the last char is dropped) */
    for (int i = BW_COLS - 1; i > cur_col; i--) grid[cur_row][i] = grid[cur_row][i - 1];
    grid[cur_row][cur_col] = c;
    if (cur_col < BW_COLS - 1) cur_col++;
}
static void backspace(void)
{
    if (cur_col > 0) {
        for (int i = cur_col - 1; i < BW_COLS - 1; i++) grid[cur_row][i] = grid[cur_row][i + 1];
        grid[cur_row][BW_COLS - 1] = ' ';
        cur_col--;
    } else if (cur_row > view_top) {
        cur_row--; cur_col = 0;     /* hop to start of the previous line */
    }
}

/* trim trailing spaces off the cursor's row into out[] */
static void row_text(int r, char *out, int cap)
{
    int n = BW_COLS;
    while (n > 0 && grid[r][n - 1] == ' ') n--;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = grid[r][i];
    out[n] = 0;
}

static void do_enter(void)
{
    char line[BW_COLS + 1];
    row_text(cur_row, line, sizeof line);
    cur_col = 0;
    cursor_down_one();             /* output/echo flows from the next row */
    if (line[0]) {
        basic_exec_line(line);
    } else if (gfx_on) {
        gfx_on = 0; bgfx_clear();  /* Enter on an empty line dismisses graphics */
    }
}

void basicwin_handle_key(char c)
{
    if (!inited) return;

    /* --- ANSI escape parser for the arrow / nav keys --- */
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; return; }
    if (esc_state == 2) {
        esc_state = 0;
        switch (c) {
            case 'A': if (cur_row > 0)            cur_row--; break;   /* up    */
            case 'B': if (cur_row < BW_ROWS - 1)  cur_row++; break;   /* down  */
            case 'C': if (cur_col < BW_COLS - 1)  cur_col++; break;   /* right */
            case 'D': if (cur_col > 0)            cur_col--; break;   /* left  */
            case 'H': cur_col = 0; break;                            /* Home  */
            case 'F': { int n = BW_COLS;                             /* End   */
                        while (n > 0 && grid[cur_row][n-1] == ' ') n--;
                        cur_col = n; } break;
            case '5': if (cur_row >= 20) cur_row -= 20; else cur_row = 0; break;     /* PgUp */
            case '6': if (cur_row < BW_ROWS - 20) cur_row += 20; else cur_row = BW_ROWS-1; break; /* PgDn */
            default: break;
        }
        return;
    }
    if (c == 0x1b) { esc_state = 1; return; }

    if (c == 0x03)                      { cur_col = 0; cursor_down_one(); } /* Ctrl-C: cancel line */
    else if (c == '\r' || c == '\n')      do_enter();
    else if (c == 0x08 || c == 0x7f)      backspace();
    else if (c >= 0x20 && c < 0x7f)       insert_char(c);
    /* other control chars ignored */
}
