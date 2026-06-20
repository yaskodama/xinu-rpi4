// device/video/wm.c — window manager: chrome + redraw loop.

#include "wm.h"
#include "video.h"

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    20            /* 1 frame every 50 ms          */

static window_t *wm_head;
static struct window *active_win;              /* last-clicked window (highlighted border) */
static void    (*wm_tick)(void);

/* Cursor overlay state — repainted on top of all windows every
 * frame so it never disappears under another redraw.  Cursor
 * coordinates are in *screen* space, not virtual desktop, so the
 * cursor stays anchored to the display when the viewport pans. */
static int cursor_x = 320;
static int cursor_y = 240;
static int cursor_visible = 1;

/* Viewport state — top-left corner of the visible camera inside
 * the WM_DESKTOP_W × WM_DESKTOP_H virtual desktop. */
static int vp_x = 0;
static int vp_y = 0;
/* Autopan defaults to OFF — the boot log needs to stay readable
 * in the shell window during early debugging.  The `autopan on`
 * shell command (re-)enables the demo cycle once input works. */
static int autopan_on = 0;

static void clamp_viewport(int sw, int sh)
{
    int max_x = WM_DESKTOP_W - sw;
    int max_y = WM_DESKTOP_H - sh;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (vp_x < 0) vp_x = 0;
    if (vp_y < 0) vp_y = 0;
    if (vp_x > max_x) vp_x = max_x;
    if (vp_y > max_y) vp_y = max_y;
}

void wm_pan(int dx, int dy)
{
    vp_x += dx;
    vp_y += dy;
    /* Clamp on next frame using the real screen size. */
}

void wm_set_viewport(int x, int y) { vp_x = x; vp_y = y; }
int  wm_view_x(void) { return vp_x; }
int  wm_view_y(void) { return vp_y; }

void wm_set_autopan(int on) { autopan_on = on ? 1 : 0; }

void wm_set_tick(void (*fn)(void))
{
    wm_tick = fn;
}

void wm_cursor_set(int x, int y, int visible)
{
    cursor_x = x;
    cursor_y = y;
    cursor_visible = visible;
}

/* 12×12 arrow cursor sprite.  '#' = white, '.' = black border,
 * ' ' = transparent.  Anchor (hot-spot) is top-left. */
static const char cursor_sprite[12][12] = {
    {'#','.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','.',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','.',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','.',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','.',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','.',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','#','.',' ',' ',' '},
    {'#','#','#','#','#','.','.','.','.',' ',' ',' '},
    {'#','#','.','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','.',' ','.','#','#','.',' ',' ',' ',' ',' '},
    {'.',' ',' ',' ','.','#','#','.',' ',' ',' ',' '},
};

static void draw_cursor(void)
{
    if (!cursor_visible) return;
    /* Cursor is in screen coords — reset the viewport so the
     * sprite always renders 1:1 onto the physical display, then
     * restore it for the next frame's window draws. */
    int save_x = video_viewport_x();
    int save_y = video_viewport_y();
    video_set_viewport(0, 0);

    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    for (int dy = 0; dy < 12; dy++) {
        int py = cursor_y + dy;
        if (py < 0 || py >= sh) continue;
        for (int dx = 0; dx < 12; dx++) {
            int px = cursor_x + dx;
            if (px < 0 || px >= sw) continue;
            char c = cursor_sprite[dy][dx];
            if (c == '#')      fill_rect(px, py, 1, 1, 0xFFFFFFFFU);
            else if (c == '.') fill_rect(px, py, 1, 1, 0xFF000000U);
        }
    }

    video_set_viewport(save_x, save_y);
}

/* ---- smooth cursor: draw straight onto the VISIBLE HDMI buffer with a 12x12
 * save/restore backing store, so the wm loop can move the pointer many times
 * between the (expensive) 20 fps full-scene flips. ---- */
extern void video_vis_save(int, int, int, int, unsigned int *);
extern void video_vis_restore(int, int, int, int, const unsigned int *);
extern void video_vis_pixel(int, int, unsigned int);
extern void video_back_save(int, int, int, int, unsigned int *);
extern void xhci_mouse_pump(void);     /* cheap event-ring drain -> cursor_x/y */
#define WM_CURSOR_STEP_MS 2
static unsigned int cur_bak[12*12];
static int          cur_bak_valid, cur_bak_x, cur_bak_y;

static void cursor_vis_hide(void)
{
    if (cur_bak_valid) { video_vis_restore(cur_bak_x, cur_bak_y, 12, 12, cur_bak); cur_bak_valid = 0; }
}
static void cursor_vis_show(void)
{
    if (!cursor_visible) return;
    cur_bak_x = cursor_x; cur_bak_y = cursor_y;
    video_vis_save(cur_bak_x, cur_bak_y, 12, 12, cur_bak); cur_bak_valid = 1;
    for (int dy = 0; dy < 12; dy++)
        for (int dx = 0; dx < 12; dx++) {
            char c = cursor_sprite[dy][dx];
            if      (c == '#') video_vis_pixel(cursor_x + dx, cursor_y + dy, 0xFFFFFFFFU);
            else if (c == '.') video_vis_pixel(cursor_x + dx, cursor_y + dy, 0xFF000000U);
        }
}

/* Re-stamp the cursor onto the visible buffer after an OUT-OF-BAND present.
 * A long-running BASIC graphics loop blocks wm_run (single thread) and flips
 * frames itself via video_present(), which wipes the cursor; this redraws it
 * (frozen at its last position) so the pointer doesn't vanish during the run. */
void wm_cursor_repaint(void)
{
    cur_bak_valid = 0;          /* the flip invalidated the cached backing */
    cursor_vis_show();
}

/* As wm_cursor_repaint, but FIRST pump the mouse so the pointer keeps moving
 * during a blocking BASIC graphics loop.  Keyboard reports drained by the pump
 * are dropped (xhci_keyboard_event guards on basic_is_running), and clicks are
 * ignored (xhci_mouse_event skips wm_pointer during a run) — so this can't
 * re-enter the interpreter. */
void wm_cursor_after_blit(void)
{
    cur_bak_valid = 0;          /* the present wiped the visible buffer */
    xhci_mouse_pump();          /* update cursor_x/y from HID motion */
    cursor_vis_show();          /* stamp the cursor at its current position */
}

/* Pump the mouse once and move ONLY the cursor (cheap 12x12 backing-store
 * update, no full-scene present) when it moved.  Shared by the wm idle loop
 * and by a blocking BASIC graphics run. */
void wm_cursor_tick(void)
{
    int px = cursor_x, py = cursor_y;
    xhci_mouse_pump();
    if (cursor_x != px || cursor_y != py) { cursor_vis_hide(); cursor_vis_show(); }
}

/* delay_ms(), but pump the cursor every WM_CURSOR_STEP_MS.  A single-threaded
 * BASIC program blocks wm_run during PAUSE/WAIT; without this the HID interrupt
 * EP runs out of armed TRBs (only 2 primed) between the ~100 ms pumps and most
 * mouse motion is lost, so the pointer appears frozen (e.g. the koch sample).
 * Slicing the wait keeps a transfer in flight and the cursor smooth. */
void wm_cursor_delay_ms(int ms)
{
    int n = 0;
    for (int t = 0; t < ms; t += WM_CURSOR_STEP_MS) {
        delay_ms(WM_CURSOR_STEP_MS);
        wm_cursor_tick();
        /* The wm idle loop is blocked while a BASIC program runs, so yield to
         * the net/HTTP/wifi procs here (~every 20 ms) — otherwise a long or
         * infinite program (e.g. koch) wedges the whole box: no ping, no HTTP,
         * no remote chainload, only a power-cycle recovers it. */
        if ((++n % 10) == 0 && wm_tick) wm_tick();
    }
}

/* The window the user last clicked (keyboard input is routed here). */
window_t *wm_active(void) { return active_win; }

void wm_add(window_t *w)
{
    w->next = 0;
    if (wm_head == 0) {
        wm_head = w;
        return;
    }
    window_t *t = wm_head;
    while (t->next) t = t->next;
    t->next = w;
}

/* ---- runtime window geometry (driven by the AIPL layout designer) ---- */
static window_t *wm_nth(int idx)
{
    window_t *w = wm_head;
    while (w && idx-- > 0) w = w->next;
    return w;
}

int wm_window_count(void)
{
    int n = 0;
    for (window_t *w = wm_head; w; w = w->next) n++;
    return n;
}

int wm_window_move(int idx, int x, int y)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (x < 0) x = 0; if (x > WM_DESKTOP_W - 16) x = WM_DESKTOP_W - 16;
    if (y < 0) y = 0; if (y > WM_DESKTOP_H - 16) y = WM_DESKTOP_H - 16;
    w->x = x; w->y = y;        /* next frame redraws at the new spot */
    return 0;
}

int wm_window_resize(int idx, int wd, int ht)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (wd >= 24) w->width  = wd;   /* keep room for the titlebar/chrome */
    if (ht >= 24) w->height = ht;
    return 0;
}

int wm_window_name(int idx, char *out, int cap)
{
    window_t *w = wm_nth(idx);
    if (!w || cap <= 0) return -1;
    int i = 0;
    while (w->title[i] && i < cap - 1) { out[i] = w->title[i]; i++; }
    out[i] = 0;
    return i;
}

int wm_window_get(int idx, int *x, int *y, int *wd, int *ht)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (x)  *x  = w->x;
    if (y)  *y  = w->y;
    if (wd) *wd = w->width;
    if (ht) *ht = w->height;
    return 0;
}

int wm_window_font(int idx, int scale)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;          /* keep glyphs sane vs. the window size */
    w->font_scale = scale;
    return 0;
}

int wm_window_fontscale(int idx)
{
    window_t *w = wm_nth(idx);
    return (w && w->font_scale > 0) ? w->font_scale : 1;
}

static void draw_chrome(window_t *w)
{
    /* outer border */
    draw_rect(w->x, w->y, w->width, w->height, w->chrome_color);

    /* title bar background (one pixel inside the border) */
    fill_rect(w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_H, w->title_bg);

    /* close button (❌) at the title-bar LEFT: a small red box with a white
     * "X".  Clicking it removes the window (see window_at_close / wm_close). */
    fill_rect(w->x + 2, w->y + 2, 8, 8, 0xFFCC4040U);
    draw_string_at(w->x + 2, w->y + 2, "X", 0xFFFFFFFFU, 0xFFCC4040U);

    /* title text — shifted right past the close button */
    draw_string_at(w->x + WM_TITLEBAR_H + 4, w->y + 2, w->title, w->title_fg, w->title_bg);

    /* separator under the title */
    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H + 1,
              w->width - 2, 1, w->chrome_color);

    /* content background */
    int cy = w->y + WM_TITLEBAR_H + 2;
    int ch = w->height - WM_TITLEBAR_H - 3;
    fill_rect(w->x + 1, cy, w->width - 2, ch, w->content_bg);

    /* resize grip: three short diagonal ticks in the bottom-right corner */
    { int gx = w->x + w->width - 4, gy = w->y + w->height - 4;
      for (int k = 0; k < 3; k++)
          fill_rect(gx - k*4, gy - k*4, 2, 2, w->chrome_color); }

    /* active (last-clicked) window: a thicker amber highlight border */
    if (w == active_win) {
        unsigned int hl = 0xFFFFD23CU;            /* amber */
        draw_rect(w->x,     w->y,     w->width,     w->height,     hl);
        draw_rect(w->x + 1, w->y + 1, w->width - 2, w->height - 2, hl);
        draw_rect(w->x + 2, w->y + 2, w->width - 4, w->height - 4, hl);
    }
}

/* ---- window drag: left-button drag on a title bar moves the window. ----
 * Driven by xhci_mouse_event (main.c) via wm_pointer() on every mouse report.
 * Cursor coords are SCREEN-space; windows live in desktop coords (+viewport). */
#define WM_RESIZE_GRAB 16                      /* bottom-right grab square */
#define WM_MIN_W       96
#define WM_MIN_H       (WM_TITLEBAR_H + 24)
static window_t *drag_win;
static int       drag_mode;                    /* 1 = move, 2 = resize */
static int       drag_off_x, drag_off_y;

static window_t *window_at_titlebar(int dx, int dy)
{
    window_t *hit = 0;                 /* later in the list = drawn on top */
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x && dx < w->x + w->width &&
            dy >= w->y && dy < w->y + WM_TITLEBAR_H)
            hit = w;
    return hit;
}

/* Topmost window whose close-button box (a TITLEBAR_H square at the title-bar
 * left) contains (dx,dy).  Hit area is generous (the full bar height). */
static window_t *window_at_close(int dx, int dy)
{
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x + 1 && dx < w->x + 1 + WM_TITLEBAR_H &&
            dy >= w->y + 1 && dy < w->y + 1 + WM_TITLEBAR_H)
            hit = w;
    return hit;
}

/* Remove `w` from the desktop (the close-button action).  The window struct
 * itself is caller-owned (often static) and is NOT freed — it just stops being
 * drawn / hit-tested.  Any worker process behind it keeps running. */
void wm_close(window_t *w)
{
    if (!w || !wm_head) return;
    if (wm_head == w) {
        wm_head = w->next;
    } else {
        window_t *p = wm_head;
        while (p->next && p->next != w) p = p->next;
        if (p->next != w) return;          /* not in the list */
        p->next = w->next;
    }
    w->next = 0;
    if (drag_win == w)  { drag_win = 0; drag_mode = 0; }
    if (active_win == w) active_win = wm_head;   /* focus whatever's left */
}

/* Topmost window whose bottom-right corner grab square contains (dx,dy). */
static window_t *window_at_resize(int dx, int dy)
{
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next) {
        int rx = w->x + w->width, ry = w->y + w->height;
        if (dx >= rx - WM_RESIZE_GRAB && dx < rx && dy >= ry - WM_RESIZE_GRAB && dy < ry)
            hit = w;
    }
    return hit;
}

/* Bring `w` to the front of the draw order (end of the list = on top). */
static void wm_raise(window_t *w)
{
    if (!wm_head || wm_head == w) { /* if head, only move when it has siblings */
        if (wm_head == w && w->next) {
            wm_head = w->next;
        } else return;
    } else {
        window_t *p = wm_head;
        while (p->next && p->next != w) p = p->next;
        if (!p->next) return;          /* not found */
        p->next = w->next;
    }
    window_t *t = wm_head;
    while (t->next) t = t->next;
    t->next = w; w->next = 0;
}

/* ---- right-click context menu -------------------------------------------
 * Opened by a right-button press (wm_menu_open(), called from xhci_mouse_event);
 * the next left press selects the item under the cursor, or closes the menu if
 * the press lands outside it.  Drawn each frame by wm_run() while open.  All
 * coords are desktop-space (same as windows). */
#define WM_MENU_ITEM_H 18
#define WM_MENU_W      140
static int menu_open, menu_x, menu_y;

static void menu_action_shell(void)
{
    extern void shellwin_new(void);          /* create/raise an on-demand shell window */
    shellwin_new();
}
static const struct { const char *label; void (*action)(void); } wm_menu_items[] = {
    { "New Shell window", menu_action_shell },
};
#define WM_MENU_N ((int)(sizeof(wm_menu_items) / sizeof(wm_menu_items[0])))

void wm_menu_open(int sx, int sy)
{
    menu_x = sx + vp_x;
    menu_y = sy + vp_y;
    if (menu_x + WM_MENU_W > WM_DESKTOP_W) menu_x = WM_DESKTOP_W - WM_MENU_W;
    if (menu_y + WM_MENU_N * WM_MENU_ITEM_H + 2 > WM_DESKTOP_H)
        menu_y = WM_DESKTOP_H - WM_MENU_N * WM_MENU_ITEM_H - 2;
    if (menu_x < 0) menu_x = 0;
    if (menu_y < 0) menu_y = 0;
    menu_open = 1;
}

static void wm_menu_draw(void)
{
    int h = WM_MENU_N * WM_MENU_ITEM_H + 2;
    fill_rect(menu_x, menu_y, WM_MENU_W, h, 0xFF202830U);
    draw_rect(menu_x, menu_y, WM_MENU_W, h, 0xFFFFD23CU);   /* amber border */
    for (int i = 0; i < WM_MENU_N; i++) {
        int iy = menu_y + 1 + i * WM_MENU_ITEM_H;
        draw_string_at(menu_x + 6, iy + 5, wm_menu_items[i].label, 0xFFFFFFFFU, 0xFF202830U);
    }
}

/* Add `w` to the desktop if it isn't there yet, then focus + raise it.  Used
 * by the context menu to pop up a window on demand. */
void wm_show(window_t *w)
{
    for (window_t *t = wm_head; t; t = t->next)
        if (t == w) { active_win = w; wm_raise(w); return; }
    wm_add(w);
    active_win = w;
    wm_raise(w);
}

void wm_pointer(int sx, int sy, int left)
{
    int dx = sx + vp_x, dy = sy + vp_y;         /* screen -> desktop */
    static int prev_left;                        /* for press-edge detection */
    int press_edge = (left && !prev_left);
    prev_left = left;
    /* When the context menu is open, a left press is consumed by it: pick the
     * item under the cursor, or just dismiss the menu if the press is outside. */
    if (menu_open) {
        if (press_edge) {
            int idx = -1;
            if (dx >= menu_x && dx < menu_x + WM_MENU_W &&
                dy >= menu_y && dy < menu_y + WM_MENU_N * WM_MENU_ITEM_H)
                idx = (dy - menu_y) / WM_MENU_ITEM_H;
            menu_open = 0;
            if (idx >= 0 && idx < WM_MENU_N && wm_menu_items[idx].action)
                wm_menu_items[idx].action();
        }
        return;
    }
    if (left) {
        if (!drag_win) {
            /* Close button (❌) at the title-bar left takes priority over drag /
             * raise: a press there removes the window. */
            if (press_edge) {
                window_t *cb = window_at_close(dx, dy);
                if (cb) { wm_close(cb); return; }
            }
            window_t *w = window_at_resize(dx, dy);     /* corner first (it's inside the window) */
            if (w) { drag_win = w; drag_mode = 2; drag_off_x = dx - (w->x + w->width);
                     drag_off_y = dy - (w->y + w->height); active_win = w; wm_raise(w); }
            else if ((w = window_at_titlebar(dx, dy)) != 0) {
                drag_win = w; drag_mode = 1; drag_off_x = dx - w->x; drag_off_y = dy - w->y; active_win = w; wm_raise(w);
            }
            else {
                /* Click inside a window BODY (not titlebar/resize corner):
                 * focus it (so the keyboard routes here) and raise it, but do
                 * NOT start a drag.  Without this, clicking the shell window's
                 * content didn't give it keyboard focus, so typed commands went
                 * to whatever window was last title-bar-clicked (e.g. BASIC). */
                window_t *hit = 0;
                for (window_t *t = wm_head; t; t = t->next)
                    if (dx >= t->x && dx < t->x + t->width &&
                        dy >= t->y && dy < t->y + t->height)
                        hit = t;                       /* topmost = last in list */
                if (hit) {
                    active_win = hit; wm_raise(hit);
                    /* Fire the window's click handler once per press (e.g. the
                     * BASIC window's toolbar buttons).  Local coords. */
                    if (press_edge && hit->on_click)
                        hit->on_click(hit, dx - hit->x, dy - hit->y);
                }
            }
        } else if (drag_mode == 2) {                    /* resize bottom-right */
            int nw = dx - drag_off_x - drag_win->x;
            int nh = dy - drag_off_y - drag_win->y;
            if (nw < WM_MIN_W) nw = WM_MIN_W;
            if (nh < WM_MIN_H) nh = WM_MIN_H;
            if (drag_win->x + nw > WM_DESKTOP_W) nw = WM_DESKTOP_W - drag_win->x;
            if (drag_win->y + nh > WM_DESKTOP_H) nh = WM_DESKTOP_H - drag_win->y;
            drag_win->width = nw; drag_win->height = nh;
        } else {                                        /* move */
            drag_win->x = dx - drag_off_x;
            drag_win->y = dy - drag_off_y;
            if (drag_win->x < 0) drag_win->x = 0;
            if (drag_win->y < 0) drag_win->y = 0;
            if (drag_win->x > WM_DESKTOP_W - 16) drag_win->x = WM_DESKTOP_W - 16;
            if (drag_win->y > WM_DESKTOP_H - 16) drag_win->y = WM_DESKTOP_H - 16;
        }
    } else {
        drag_win = 0; drag_mode = 0;                     /* button released */
    }
}

/* Persistent WiFi status badge pinned to the screen's bottom-right corner.
 * Drawn in screen space (viewport 0,0) so it stays put while the desktop pans.
 * Connected: GREEN signal bars + the joined SSID + the DHCP IP address.
 * Down: a compact dim-grey badge with "----". */
static void draw_wifi_badge(int sw, int sh)
{
    extern int wifi_connected(void);
    extern const char *wifi_ssid(void);
    extern void wifi_ipaddr(unsigned char *o);

    unsigned int bg = 0xFF101820U;                              /* dark panel   */
    int connected = wifi_connected();

    if (!connected) {
        unsigned int fg = 0xFF777777U;                          /* grey         */
        int bw = 58, bh = 18, bx = sw - bw - 6, by = sh - bh - 6;
        fill_rect(bx, by, bw, bh, bg);
        fill_rect(bx, by, bw, 1, 0xFF2A3A4AU);
        int base_y = by + bh - 4;
        for (int i = 0; i < 4; i++) { int barh = 3 + i * 3; fill_rect(bx + 4 + i * 4, base_y - barh, 3, barh, 0xFF555555U); }
        video_set_text_scale(1);
        draw_string_at(bx + 24, by + 5, "----", fg, bg);
        return;
    }

    /* Connected — build the "a.b.c.d" IP string (no libc in freestanding). */
    unsigned char ip[4]; wifi_ipaddr(ip);
    char ipbuf[20]; int p = 0;
    for (int o = 0; o < 4; o++) {
        int v = ip[o];
        if (v >= 100)     { ipbuf[p++] = (char)('0' + v / 100); ipbuf[p++] = (char)('0' + (v / 10) % 10); ipbuf[p++] = (char)('0' + v % 10); }
        else if (v >= 10) { ipbuf[p++] = (char)('0' + v / 10);  ipbuf[p++] = (char)('0' + v % 10); }
        else                ipbuf[p++] = (char)('0' + v);
        if (o < 3) ipbuf[p++] = '.';
    }
    ipbuf[p] = 0;

    const char *ssid = wifi_ssid();
    int sl = 0; while (ssid[sl]) sl++;

    unsigned int fg = 0xFF22DD55U;                              /* green        */
    int barsw = 4 + 4 * 4;                                      /* bars zone    */
    int textw = (sl > p ? sl : p) * 8;
    int bw = barsw + textw + 12, bh = 26;
    if (bw > sw - 8) bw = sw - 8;
    int bx = sw - bw - 6, by = sh - bh - 6;

    fill_rect(bx, by, bw, bh, bg);                              /* panel        */
    fill_rect(bx, by, bw, 1, 0xFF2A6A3AU);                      /* green top    */

    int base_y = by + bh - 5;                                  /* signal bars  */
    for (int i = 0; i < 4; i++) { int barh = 3 + i * 3; fill_rect(bx + 4 + i * 4, base_y - barh, 3, barh, fg); }

    video_set_text_scale(1);
    draw_string_at(bx + barsw + 4, by + 3,  ssid,  fg,          bg);   /* SSID  */
    draw_string_at(bx + barsw + 4, by + 14, ipbuf, 0xFFB8F0CCU, bg);   /* IP    */
}

void wm_run(void)
{
    unsigned int frame = 0;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

    /* Render off-screen so the per-frame full-screen wipe + redraw never
     * shows as flicker; each finished frame is flipped to the visible
     * framebuffer in one pass via video_present().  Falls back to direct
     * drawing if the buffer can't be allocated. */
    video_enable_backbuffer();

    /* Initial desktop wipe (screen-space, viewport bypass). */
    video_set_viewport(0, 0);
    fill_rect(0, 0, sw, sh, DESKTOP_BG);

    for (;;) {
        if (wm_tick) wm_tick();

        /* Auto-pan demo: cycle the viewport through the four
         * corners of the virtual desktop on a ~24 s loop.  Until
         * USPi delivers real arrow-key / mouse-button input, this
         * is the only way to actually *see* the bigger desktop
         * scroll.  Phase boundaries: 0..120 right, 120..240 down,
         * 240..360 left, 360..480 up.  At 20 fps frame=480 ≈ 24 s. */
        if (autopan_on) {
            unsigned int phase = (frame / 120) & 3;
            int t = (int)(frame % 120);
            int max_x = WM_DESKTOP_W - sw;
            int max_y = WM_DESKTOP_H - sh;
            if (max_x < 0) max_x = 0;
            if (max_y < 0) max_y = 0;
            switch (phase) {
                case 0: vp_x = (max_x * t) / 120;       vp_y = 0;                       break;
                case 1: vp_x = max_x;                    vp_y = (max_y * t) / 120;      break;
                case 2: vp_x = max_x - (max_x * t)/120;  vp_y = max_y;                  break;
                case 3: vp_x = 0;                        vp_y = max_y - (max_y * t)/120;break;
            }
        }
        clamp_viewport(sw, sh);

        /* Repaint the visible screen with the desktop background
         * before any windows — the auto-pan shifts the camera so
         * stale pixels from the previous frame need to be cleared. */
        video_set_viewport(0, 0);
        fill_rect(0, 0, sw, sh, DESKTOP_BG);

        /* Now switch to the panned camera and draw all windows in
         * virtual desktop coordinates. */
        video_set_viewport(vp_x, vp_y);
        for (window_t *w = wm_head; w; w = w->next) {
            video_set_text_scale(1);          /* chrome/title always 1x */
            draw_chrome(w);
            if (w->draw_content) {
                /* Scale draw_string_at glyphs to this window's font size; the
                 * window's own draw fn scales its line/column spacing to match. */
                video_set_text_scale(w->font_scale > 0 ? w->font_scale : 1);
                w->draw_content(w, frame);
                video_set_text_scale(1);
            }
        }

        if (menu_open) wm_menu_draw();     /* context menu on top of the windows */

        /* WiFi status badge — screen-space (bottom-right), on top of everything
         * but under the cursor.  Restore the panned viewport afterwards so the
         * cursor composite below is unaffected. */
        video_set_viewport(0, 0);
        draw_wifi_badge(sw, sh);
        video_set_viewport(vp_x, vp_y);

        /* Composite the cursor INTO the finished backbuffer *before* the flip
         * so the pointer arrives in the same atomic present as the scene.  The
         * old "present (wipes cursor) then redraw on the visible buffer" made
         * the cursor vanish for the duration of every present → a ~20 Hz blink.
         * First grab the cursor-free pixels under the pointer from the
         * backbuffer so the fast sub-loop below can still move it smoothly via
         * the visible-buffer backing store. */
        cur_bak_valid = 0;
        if (cursor_visible) {
            video_back_save(cursor_x, cursor_y, 12, 12, cur_bak);
            cur_bak_x = cursor_x; cur_bak_y = cursor_y; cur_bak_valid = 1;
            draw_cursor();         /* stamp into the backbuffer */
        }
        video_present();   /* atomic flip: scene + cursor together (no blink) */

        /* Fast cursor sub-loop for the rest of the frame interval: pump the
         * mouse and move ONLY the cursor (backing store) at ~330 Hz, so the
         * pointer is smooth even though the full scene repaints at 20 fps. */
        for (int t = 0; t < 1000 / DEFAULT_FPS; t += WM_CURSOR_STEP_MS) {
            delay_ms(WM_CURSOR_STEP_MS);
            wm_cursor_tick();                       /* tight: drain HID -> cursor_x/y */
            /* let the net/shell/wifi procs run a few times per frame, not every
             * 2 ms (proc_yield in the tick otherwise throttles cursor polling). */
            if ((t % 10) == 0 && wm_tick) wm_tick();
        }
        frame++;
    }
}
