// device/video/basicwin.c — wm-managed BASIC window as a full-screen text
// editor.
//
// No prompt — just a blinking-free block cursor in a large space-padded text
// grid (~10 screens of scrollback).  Arrow keys move the cursor freely; typing
// inserts at the cursor; Enter runs the cursor's line through basic_exec_line()
// and the interpreter's output (bw_emit) flows from the cursor like a terminal.
// The view auto-scrolls to keep the cursor visible; PageUp/PageDown and the
// up/down arrows at the edges scroll the 10-screen buffer.
//
// MULTI-INSTANCE: up to BASICWIN_N BASIC windows, each with its OWN text grid,
// cursor and interpreter instance (bs[i] in basic.c).  Window 0 is the primary
// `basic_win` wired at boot; the right-click menu's "New BASIC window" builds
// the rest via basicwin_new().  The WM is single-threaded, so before dispatching
// a key / click / draw to window i we set bw_curi = i and basic_select(i); every
// per-window field below resolves through ctx[bw_curi], and every bs[] macro in
// basic.c resolves through basic_curi() — so the two stay in lock-step.

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
extern void basic_set_button(void (*fn)(int, const char *));
extern void basic_set_btn(int (*fn)(int));
extern void basic_set_buttons_reset(void (*fn)(void));
extern void basic_break(void);             /* request a RUN-loop break */
extern void basic_select(int inst);        /* pick which bs[] instance is current */
extern int  xhci_poll_ctrl_c(void);        /* polls "is Ctrl+C held?" */
extern void wm_cursor_after_blit(void);    /* pump mouse + re-stamp cursor */

#define BW_COLS   96            /* chars per row (fixed grid)              */
#define BW_ROWS   360           /* ~10 screens of scrollback              */
#define BW_N      BASICWIN_N    /* how many independent BASIC windows      */

/* ---- per-window state ----------------------------------------------------
 * Everything that used to be a file-scope global now lives in one struct,
 * instanced per window in ctx[].  The function bodies below are unchanged: the
 * macros after the array make `grid`, `cur_row`, … resolve to ctx[bw_curi]. */
struct bw_ctx {
    char grid[BW_ROWS][BW_COLS + 1];  /* space-padded; trailing NUL for drawing */
    int  cur_row, cur_col;            /* cursor in buffer coords                */
    int  view_top;                    /* first buffer row shown                 */
    int  inited;
    int  esc_state;                   /* ANSI escape parser: 0 none,1 ESC,2 ESC[ */
    int  gfx_on;                      /* 1 once a program enters graphics mode   */
    int  gfx_dirty;                   /* 1 if the gfx list changed since last flip */
};
static struct bw_ctx ctx[BW_N];
static int bw_curi;                   /* active instance for the macros below   */

#define grid       (ctx[bw_curi].grid)
#define cur_row    (ctx[bw_curi].cur_row)
#define cur_col    (ctx[bw_curi].cur_col)
#define view_top   (ctx[bw_curi].view_top)
#define inited     (ctx[bw_curi].inited)
#define esc_state  (ctx[bw_curi].esc_state)
#define gfx_on     (ctx[bw_curi].gfx_on)
#define gfx_dirty  (ctx[bw_curi].gfx_dirty)

/* Window 0 is the primary, wired at boot (others reference &basic_win); windows
 * 1.. are built on demand by basicwin_new(). */
window_t basic_win;
static window_t basic_win_x[BW_N - 1];
static int extra_count;                /* extra BASIC windows built (0..BW_N-1)  */

static window_t *win_of(int i) { return i == 0 ? &basic_win : &basic_win_x[i - 1]; }
static int inst_of(window_t *w)
{
    if (w == &basic_win) return 0;
    for (int i = 0; i < BW_N - 1; i++) if (w == &basic_win_x[i]) return i + 1;
    return -1;
}

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
    { "maze",  "RUN \"maze\""   },
    { "glass", "RUN \"glass\""  },
    { "flight","RUN \"flight\"" },
    { "rescue","RUN \"rescue\"" },
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

/* ---- program buttons: BASIC `BUTTON n,"label"[,line]` -------------------
 * Drawn as a small strip OVERLAID on the top edge of the graphics area (so it
 * never steals canvas space from a program like koch).  A click bumps the per-
 * button counter that BTN(n) / the event dispatcher drains. */
#define PBTN_MAX 8
#define BW_PBTN_H 15
static char pbtn_label[PBTN_MAX][24];
static int  pbtn_present[PBTN_MAX];
static int  pbtn_clicks[PBTN_MAX];

static int pbtn_count(void) { int c = 0; for (int i = 0; i < PBTN_MAX; i++) if (pbtn_present[i]) c++; return c; }

/* Window-local rect of present program button n; returns 0 if not present. */
static int pbtn_rect(int win_w, int n, int *bx, int *by, int *bw, int *bh)
{
    if (n < 0 || n >= PBTN_MAX || !pbtn_present[n]) return 0;
    int cnt = pbtn_count(); if (cnt < 1) return 0;
    int idx = 0; for (int i = 0; i < n; i++) if (pbtn_present[i]) idx++;
    int avail = win_w - 4 - BW_BTN_GAP * (cnt - 1);
    int w = avail / cnt; if (w < 1) w = 1;
    *bw = w; *bh = BW_PBTN_H;
    *bx = 2 + idx * (w + BW_BTN_GAP);
    *by = WM_TITLEBAR_H + BW_TOOLBAR_H + 1;
    return 1;
}
/* interpreter seams (wired in basicwin_init). */
static void bw_button(int n, const char *label)
{
    if (n < 0 || n >= PBTN_MAX) return;
    int i = 0; for (; label[i] && i < 23; i++) pbtn_label[n][i] = label[i];
    pbtn_label[n][i] = 0; pbtn_present[n] = 1;
}
static int  bw_btn(int n) { if (n < 0 || n >= PBTN_MAX) return 0; int c = pbtn_clicks[n]; pbtn_clicks[n] = 0; return c; }
static void bw_buttons_reset(void) { for (int i = 0; i < PBTN_MAX; i++) { pbtn_present[i] = 0; pbtn_clicks[i] = 0; pbtn_label[i][0] = 0; } }

static void bw_draw_pbtns(window_t *self)
{
    for (int n = 0; n < PBTN_MAX; n++) {
        int bx, by, bw, bh;
        if (!pbtn_rect(self->width, n, &bx, &by, &bw, &bh)) continue;
        int ax = self->x + bx, ay = self->y + by;
        unsigned int bg = 0xFF9A5A12U;                    /* amber program button */
        fill_rect(ax, ay, bw, bh, bg);
        draw_rect(ax, ay, bw, bh, 0xFFFFC766U);
        int lw = bw_strlen(pbtn_label[n]) * FONT_WIDTH;
        int tx = ax + (bw - lw) / 2; if (tx < ax + 1) tx = ax + 1;
        int ty = ay + (bh - FONT_HEIGHT) / 2;
        draw_string_scaled(tx, ty, pbtn_label[n], 0xFFFFFFFFU, bg, 1);
    }
}

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
    if (mode & 2) { bgfx_clear(); gfx_on = 1; gfx_dirty = 1; }
    if (mode == 0) clear_all();          /* bare CLS == text clear */
}

/* Repaint the BASIC graphics area cleanly and flip it to screen — lets the
 * single-threaded interpreter show animation between PAUSEs (the wm render loop
 * is blocked while a program RUNs). */
static void bw_present_gfx(void)
{
    window_t *self = win_of(bw_curi);
    int gx, gy, gw, gh;
    bw_gfx_rect(self, &gx, &gy, &gw, &gh);
    fill_rect(gx, gy, gw, gh, self->content_bg);
    bgfx_render(gx, gy, gw, gh);
    bw_draw_pbtns(self);      /* keep program buttons painted during a RUN */
    video_present();
    wm_cursor_after_blit();   /* flip wiped the cursor — pump mouse + re-stamp */
}

/* ===== `wine` shell command: 3-D wireframe wine-glass in the GRAPHICS window
 * (gfx_* list / gfx_win in main.c) — the SAME revolved profile as the BASIC
 * `glass` sample, spun a little about X/Y/Z for 100 frames, then it stops.
 * Driven from shell/shell.c. ===================================== */
static double w_sin(double x)
{
    while (x >  3.14159265358979) x -= 6.28318530717959;
    while (x < -3.14159265358979) x += 6.28318530717959;
    double x2 = x * x, t = x, s = x;
    for (int n = 1; n < 8; n++) { t *= -x2 / (double)((2*n) * (2*n + 1)); s += t; }
    return s;
}
static double w_cos(double x) { return w_sin(x + 1.57079632679490); }

void basicwin_wine(void)
{
    extern void wm_cursor_delay_ms(int);
    extern void gfxwin_present(void);                /* raise + flip the Graphics window */
    extern void gfxwin_rect(int *, int *, int *, int *);

    int gx, gy, gw, gh; gfxwin_rect(&gx, &gy, &gw, &gh);  /* window-local content size */
    int cx = gw / 2, cy = gh / 2;
    double scl = (double)gh * 0.9;                   /* fit the glass to the window */

    /* revolved wine-glass profile (8 rings) — identical to BASIC `glass`. */
    static const double YP[8] = { 0, 0.1, 0.15, 1.1, 1.2, 1.5, 1.9, 2.2 };
    static const double RP[8] = { 0.6, 0.6, 0.08, 0.08, 0.3, 0.5, 0.55, 0.45 };
    const int NP = 8, NA = 12;
    double CT[12], ST[12];
    for (int j = 0; j < NA; j++) {
        double a = (double)j * 2.0 * 3.14159265358979 / NA;
        CT[j] = w_cos(a); ST[j] = w_sin(a);
    }
    for (int f = 0; f < 100; f++) {                 /* 100 small rotations, then stop */
        double rx = f * 0.08, ry = f * 0.11, rz = f * 0.05;
        double cxa = w_cos(rx), sxa = w_sin(rx);
        double cya = w_cos(ry), sya = w_sin(ry);
        double cza = w_cos(rz), sza = w_sin(rz);
        int VX[96], VY[96], VK[96];
        for (int i = 0; i < NP; i++) {
            double ri = RP[i], by = YP[i] - 1.1;
            for (int j = 0; j < NA; j++) {
                double bx = ri * CT[j], bz = ri * ST[j];
                double y1 = by * cxa - bz * sxa, z1 = by * sxa + bz * cxa;
                double x2 = bx * cya + z1 * sya, z2 = -bx * sya + z1 * cya;
                double wx = x2 * cza - y1 * sza, wy = x2 * sza + y1 * cza, wz = z2 + 3.0;
                int idx = i * NA + j, ok = (wz > 0.5);
                VK[idx] = ok;
                if (ok) { VX[idx] = cx + (int)(scl * wx / wz); VY[idx] = cy - (int)(scl * wy / wz); }
                else    { VX[idx] = 0; VY[idx] = 0; }
            }
        }
        gfx_clear();
        for (int i = 0; i < NP; i++)            /* angular rings (magenta) */
            for (int j = 0; j < NA; j++) {
                int j2 = (j + 1) % NA, k1 = i * NA + j, k2 = i * NA + j2;
                if (VK[k1] && VK[k2]) gfx_line(VX[k1], VY[k1], VX[k2], VY[k2], 5);
            }
        for (int i = 0; i < NP - 1; i++)        /* verticals (cyan) */
            for (int j = 0; j < NA; j++) {
                int k1 = i * NA + j, k2 = (i + 1) * NA + j;
                if (VK[k1] && VK[k2]) gfx_line(VX[k1], VY[k1], VX[k2], VY[k2], 3);
            }
        gfxwin_present();
        wm_cursor_delay_ms(120);
    }
}

static void bw_pause(int ms)
{
    extern void wm_cursor_delay_ms(int);
    /* Only flip the gfx when it actually changed since the last flip — an idle
     * WAIT loop (e.g. koch waiting for a button) must NOT keep repainting, which
     * looked like the screen "kept moving" / flickering. */
    if (gfx_on && gfx_dirty) { bw_present_gfx(); gfx_dirty = 0; }
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
    gfx_on = 1; gfx_dirty = 1; bgfx_line(x1, y1, x2, y2, color);
    /* A deep redraw (e.g. koch level 10 ~ 4^10 segments) runs with no WAIT, so
     * pump the cursor every N segments to keep the pointer alive there too. */
    if ((++seg & 63) == 0) wm_cursor_tick();
}
static void bw_circle(int cx, int cy, int r, int color)
{ gfx_on = 1; gfx_dirty = 1; bgfx_circle(cx, cy, r, color); }
static void bw_plot(int x, int y, int ch)
{ (void)ch; gfx_on = 1; gfx_dirty = 1; bgfx_line(x, y, x, y, 7); }   /* 1px dot */
static int  bw_gfx_active(void) { return gfx_on; }

/* Toolbar click: find the button under the local cursor, echo its command,
 * then run it through the interpreter (output flows from the cursor). */
static void bw_on_click(window_t *self, int lx, int ly)
{
    int i = inst_of(self);
    if (i < 0) return;
    bw_curi = i; basic_select(i);
    if (!inited) return;
    /* program BUTTONs first: a click registers an event (and consumes any
     * pending Ctrl-C so it doesn't break the running program). */
    for (int n = 0; n < PBTN_MAX; n++) {
        int bx, by, bw, bh;
        if (!pbtn_rect(self->width, n, &bx, &by, &bw, &bh)) continue;
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
            pbtn_clicks[n]++;
            xhci_poll_ctrl_c();          /* swallow any break this press queued */
            return;
        }
    }
    for (int b = 0; b < BW_NBTN; b++) {
        int bx, by, bw, bh;
        bw_btn_rect(b, self->width, &bx, &by, &bw, &bh);
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
            bw_emit(bw_btns[b].cmd);
            bw_putc('\n');
            basic_exec_line(bw_btns[b].cmd);
            return;
        }
    }
}

/* Bind one instance's interpreter (program/vars cleared) — the seams are global
 * and set once by basicwin_init(); they always act on the current bw_curi. */
static void bw_init_inst(int i)
{
    bw_curi = i; basic_select(i);
    clear_all();
    esc_state = 0;
    gfx_on = 0;
    inited = 1;
    basic_init();
}

void basicwin_init(void)
{
    bw_curi = 0; basic_select(0);
    clear_all();
    esc_state = 0;
    gfx_on = 0;
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
    basic_set_button(bw_button);            /* BUTTON n,"label" -> on-screen button */
    basic_set_btn(bw_btn);                  /* BTN(n) -> clicks since last read */
    basic_set_buttons_reset(bw_buttons_reset); /* clear program buttons on RUN */
    basic_win.on_click = bw_on_click;       /* wire the toolbar */
    /* No prompt — leave the cursor at the top-left of an empty page. */
}

/* Create (or, once all are built, raise) an on-demand BASIC window — bound to
 * the right-click context menu's "New BASIC window".  Each window cascades down
 * from the primary and gets its OWN interpreter instance. */
void basicwin_new(void)
{
    extern void wm_show(window_t *);

    if (extra_count >= BW_N - 1) {             /* all built — just raise the newest */
        wm_show(&basic_win_x[BW_N - 2]);
        return;
    }

    int i = extra_count;                       /* 0-based extra index */
    int inst = i + 1;                          /* interpreter instance 1.. */
    window_t *w = &basic_win_x[i];

    /* Cascade so a new window doesn't land exactly on the primary. */
    w->x = 40 + inst * 28;
    w->y = 60 + inst * 26;
    w->width  = 618;
    w->height = 319;
    w->font_scale = 1;

    char nm[8] = "BASIC ?";                    /* "BASIC 2" / "BASIC 3" / "BASIC 4" */
    nm[6] = (char)('2' + i);
    int k; for (k = 0; k < WM_TITLE_MAX && nm[k]; k++) w->title[k] = nm[k];
    w->title[k] = 0;

    w->chrome_color = 0xFF40A0FFU;
    w->title_bg     = 0xFF103060U;
    w->title_fg     = 0xFFFFFFFFU;
    w->content_bg   = 0xFF000810U;
    w->draw_content = basicwin_draw;
    w->on_click     = bw_on_click;

    bw_init_inst(inst);

    extra_count++;
    wm_show(w);
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
    int inst = inst_of(self);
    if (inst < 0) return;
    bw_curi = inst; basic_select(inst);

    bw_draw_toolbar(self);
    bw_draw_pbtns(self);                     /* program BUTTONs (overlay strip) */
    int fs = self->font_scale > 0 ? self->font_scale : 1;
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + BW_TOOLBAR_H + 4;
    const int line_h = (FONT_HEIGHT + 1) * fs;
    int vr = visible_rows(self, fs);

    /* keep the cursor inside the visible window */
    if (cur_row < view_top) view_top = cur_row;
    if (cur_row >= view_top + vr) view_top = cur_row - vr + 1;
    if (view_top < 0) view_top = 0;

    /* Only draw as many columns as fit in the window: each glyph (incl. the
     * trailing space padding) paints content_bg, so drawing the full BW_COLS
     * grid would spill the black background past the window's right edge
     * (BW_COLS*8 = 768 px ≫ the ~618 px window). */
    int maxcols = (self->width - 6) / (FONT_WIDTH * fs);
    if (maxcols < 1) maxcols = 1;
    if (maxcols > BW_COLS) maxcols = BW_COLS;

    char line[BW_COLS + 1];
    for (int i = 0; i < vr; i++) {
        int r = view_top + i;
        if (r >= BW_ROWS) break;
        int n = 0;
        for (; n < maxcols && grid[r][n]; n++) line[n] = grid[r][n];
        line[n] = 0;
        draw_string_scaled(cx, cy + i * line_h, line, 0xFFB6FFB6U, self->content_bg, fs);
    }

    /* block cursor at (cur_row,cur_col) if on screen */
    int crow = cur_row - view_top;
    if (crow >= 0 && crow < vr && cur_col < maxcols) {
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

/* Key handler body — operates on the current bw_curi (set by the callers). */
static void bw_key(char c)
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

void basicwin_handle_key(char c)            /* primary BASIC window (instance 0) */
{
    bw_curi = 0; basic_select(0);
    bw_key(c);
}

/* Route a keypress to an on-demand BASIC window if `aw` is one of them. */
int basicwin_route_key(window_t *aw, char c)
{
    int i = inst_of(aw);
    if (i <= 0) return 0;                    /* not an extra BASIC window */
    bw_curi = i; basic_select(i);
    bw_key(c);
    return 1;
}
