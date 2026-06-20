// device/video/softkbd.c — on-screen QWERTY keyboard.
//
// Five-row layout with key widths picked so each row fits cleanly inside the
// window's content area.  Each row is an array of key labels (1-char each for
// letters / digits, longer for specials like "Shift" / "Space" / "Bksp").
// Special keys get a wider cell proportional to a base unit.
//
// Clicking a key types into the current KEYBOARD TARGET window (wm_kbd_target()):
// the window the user last focused that isn't the soft keyboard itself — so the
// keyboard's own clicks don't steal focus.  Characters are dispatched through
// xhci_keyboard_event(), the same path the USB keyboard uses, so they reach the
// active shell / BASIC window exactly as a real keystroke would.
//
// Modifiers: Shift and Ctrl are one-shot (apply to the next key, then clear);
// Caps is a lock.  Active modifiers are highlighted so their state is visible.

#include "softkbd.h"
#include "video.h"

extern void xhci_keyboard_event(char c);   /* shared key-routing path (main.c) */

window_t softkbd_win;

/* One row = sequence of (label, width_in_units).  Width 0 means
 * use the row's default 1-unit cell.  A NULL label terminates. */
typedef struct {
    const char *label;
    int         w;   /* in base units; 0 == 1u */
} key_t;

static const key_t row0[] = {
    {"`",  0}, {"1", 0}, {"2", 0}, {"3", 0}, {"4", 0},
    {"5",  0}, {"6", 0}, {"7", 0}, {"8", 0}, {"9", 0},
    {"0",  0}, {"-", 0}, {"=", 0}, {"Bksp", 2}, {0,0}
};
static const key_t row1[] = {
    {"Tab", 2}, {"q", 0}, {"w", 0}, {"e", 0}, {"r", 0}, {"t", 0},
    {"y",   0}, {"u", 0}, {"i", 0}, {"o", 0}, {"p", 0},
    {"[",   0}, {"]", 0}, {0,0}
};
static const key_t row2[] = {
    {"Caps", 2}, {"a", 0}, {"s", 0}, {"d", 0}, {"f", 0}, {"g", 0},
    {"h",    0}, {"j", 0}, {"k", 0}, {"l", 0}, {";", 0}, {"'", 0},
    {"Ret",  2}, {0,0}
};
static const key_t row3[] = {
    {"Shift", 2}, {"z", 0}, {"x", 0}, {"c", 0}, {"v", 0}, {"b", 0},
    {"n",     0}, {"m", 0}, {",", 0}, {".", 0}, {"/", 0},
    {"Shift", 2}, {0,0}
};
static const key_t row4[] = {
    {"Ctrl", 2}, {"Alt", 2}, {"Space", 9}, {"Alt", 2}, {"Ctrl", 2}, {0,0}
};

static const key_t *rows[5] = { row0, row1, row2, row3, row4 };

/* ---- modifier state (one-shot Shift/Ctrl; Caps locks) ---- */
static int shift_on;        /* one-shot: set by Shift, cleared after next key */
static int caps_on;         /* Caps lock toggle                               */
static int ctrl_on;         /* one-shot: set by Ctrl, cleared after next key   */

static int row_unit_count(const key_t *row)
{
    int n = 0;
    for (const key_t *k = row; k->label; k++) n += k->w ? k->w : 1;
    return n;
}

static int sk_streq(const char *a, const char *b)
{ while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b; }

/* Shifted variant of a digit / symbol key (US layout). */
static char sk_shift_sym(char c)
{
    switch (c) {
        case '`': return '~';  case '1': return '!';  case '2': return '@';
        case '3': return '#';  case '4': return '$';  case '5': return '%';
        case '6': return '^';  case '7': return '&';  case '8': return '*';
        case '9': return '(';  case '0': return ')';  case '-': return '_';
        case '=': return '+';  case '[': return '{';  case ']': return '}';
        case ';': return ':';  case '\'': return '"'; case ',': return '<';
        case '.': return '>';  case '/': return '?';
        default:  return c;
    }
}

static void draw_key(int x, int y, int w, int h, const char *lbl,
                     unsigned int face, unsigned int border, unsigned int fg)
{
    /* face + border rectangle */
    fill_rect(x, y, w, h, face);
    draw_rect(x, y, w, h, border);

    /* centred glyph label.  draw_string_at is 8 px / char and
     * caller is responsible for staying inside the cell. */
    int len = 0;
    while (lbl[len]) len++;
    int gw = len * FONT_WIDTH;
    int gx = x + (w - gw) / 2;
    int gy = y + (h - FONT_HEIGHT) / 2;
    if (gx < x + 2) gx = x + 2;
    draw_string_at(gx, gy, lbl, fg, face);
}

/* Is this special label an active modifier (so it should be highlighted)? */
static int sk_mod_active(const char *lbl)
{
    if (sk_streq(lbl, "Shift")) return shift_on;
    if (sk_streq(lbl, "Caps"))  return caps_on;
    if (sk_streq(lbl, "Ctrl"))  return ctrl_on;
    return 0;
}

/* Shared geometry: compute the content rect + base cell size for (self).
 * Returns 0 if the window is too small to render. */
static int sk_geom(window_t *self, int *cx0, int *cy0, int *cw,
                   int *row_h, int *cell_h, int *unit_w)
{
    *cx0 = self->x + 4;
    *cy0 = self->y + WM_TITLEBAR_H + 4;
    *cw  = self->width  - 8;
    int ch = self->height - WM_TITLEBAR_H - 7;
    int row_count = 5;
    if (ch < row_count * 12) return 0;
    *row_h  = ch / row_count;
    *cell_h = *row_h - 2;
    int max_units = 0;
    for (int r = 0; r < row_count; r++) {
        int n = row_unit_count(rows[r]);
        if (n > max_units) max_units = n;
    }
    if (max_units == 0) return 0;
    *unit_w = *cw / max_units;
    if (*unit_w < 8) *unit_w = 8;
    return 1;
}

void softkbd_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    int cx0, cy0, cw, row_h, cell_h, unit_w;
    if (!sk_geom(self, &cx0, &cy0, &cw, &row_h, &cell_h, &unit_w)) return;

    unsigned int face     = 0xFF202830U;
    unsigned int face_mod = 0xFFB07020U;    /* active modifier highlight */
    unsigned int border   = 0xFF608090U;
    unsigned int fg       = 0xFFE8F0F8U;

    for (int r = 0; r < 5; r++) {
        int n_units = row_unit_count(rows[r]);
        int row_w = n_units * unit_w;
        int x = cx0 + (cw - row_w) / 2;
        int y = cy0 + r * row_h;
        for (const key_t *k = rows[r]; k->label; k++) {
            int kw = (k->w ? k->w : 1) * unit_w;
            int pad = 2;
            unsigned int f = sk_mod_active(k->label) ? face_mod : face;
            draw_key(x + pad, y, kw - 2 * pad, cell_h, k->label, f, border, fg);
            x += kw;
        }
    }
}

/* Hit-test: which key label is under window-local (lx,ly)?  NULL if none.
 * on_click is handed coords relative to self->x/self->y, so translate sk_geom's
 * absolute origin back to local by subtracting self->x/self->y. */
static const char *softkbd_key_at(window_t *self, int lx, int ly)
{
    int cx0, cy0, cw, row_h, cell_h, unit_w;
    if (!sk_geom(self, &cx0, &cy0, &cw, &row_h, &cell_h, &unit_w)) return 0;
    cx0 -= self->x; cy0 -= self->y;          /* absolute -> window-local */

    if (ly < cy0) return 0;
    int r = (ly - cy0) / row_h;
    if (r < 0 || r >= 5) return 0;
    if (ly >= cy0 + r * row_h + cell_h) return 0;   /* in the inter-row gap */

    int n_units = row_unit_count(rows[r]);
    int row_w = n_units * unit_w;
    int x = cx0 + (cw - row_w) / 2;
    for (const key_t *k = rows[r]; k->label; k++) {
        int kw = (k->w ? k->w : 1) * unit_w;
        if (lx >= x + 2 && lx < x + kw - 2) return k->label;
        x += kw;
    }
    return 0;
}

/* Map a clicked key to a character (honouring the modifiers) and dispatch it to
 * the current keyboard-target window, then clear the one-shot modifiers. */
void softkbd_on_click(window_t *self, int lx, int ly)
{
    const char *lbl = softkbd_key_at(self, lx, ly);
    if (!lbl) return;

    /* modifier keys: toggle, emit nothing */
    if (sk_streq(lbl, "Shift")) { shift_on = !shift_on; return; }
    if (sk_streq(lbl, "Caps"))  { caps_on  = !caps_on;  return; }
    if (sk_streq(lbl, "Ctrl"))  { ctrl_on  = !ctrl_on;  return; }
    if (sk_streq(lbl, "Alt"))   { return; }              /* unused */

    char c;
    if      (sk_streq(lbl, "Space")) c = ' ';
    else if (sk_streq(lbl, "Tab"))   c = '\t';
    else if (sk_streq(lbl, "Ret"))   c = '\r';
    else if (sk_streq(lbl, "Bksp"))  c = 0x08;
    else {
        c = lbl[0];                                      /* single-glyph key */
        if (c >= 'a' && c <= 'z') {
            int upper = shift_on ^ caps_on;              /* Shift XOR CapsLock */
            if (ctrl_on)      c = (char)((c - 'a' + 1)); /* Ctrl-A..Z -> 1..26 */
            else if (upper)   c = (char)(c - 'a' + 'A');
        } else if (shift_on) {
            c = sk_shift_sym(c);
        }
    }

    shift_on = 0;            /* one-shot modifiers consumed */
    ctrl_on  = 0;
    xhci_keyboard_event(c);  /* same routing as the USB keyboard */
}
