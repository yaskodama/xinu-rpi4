// device/video/avm.c — AIPL actor-bytecode VM ("AVM") + a "Blender" polygon
// display for the Pi 4 kernel.  Loads an .avm module (magic AVM1) posted over
// HTTP (POST /actor/loadvm, chunked like /chainload), runs its actors on a
// dedicated kernel thread, and draws their cls()/line()/tri() output into a
// "VM graphics" window — the same opcodes + 16-colour palette as the Pi 3
// (xinu-raz) so the SAME compiled .avm renders identically on both boards.
//
// Self-contained: it does NOT use the kernel's actor.c — it carries its own
// tiny object table + FIFO message queue + scheduler, which is all the AVM
// send/wait/spawn model needs.

#include "video.h"
#include "wm.h"
#include "proc.h"
#include "fat32.h"

/* ---- small freestanding helpers (no libc) ---- */
/* The VM is driven COOPERATIVELY from the WM redraw (avm_tick, one VM frame per
 * WM frame) — no separate thread, no busy-wait — so it can never starve the WM
 * or network.  vm_frame_done marks the WAIT (frame boundary) that ends a tick. */
static volatile int vm_frame_done;
static int          vm_wait_ms;             /* the WAIT() that ended the last frame */
static void avm_tick(void);                 /* forward decl (used by vmgfx_draw) */
void        avm_ctl(int cmd);               /* forward decl (used by vmgfx_click) */
static int  a_abs(int v) { return v < 0 ? -v : v; }
static unsigned long avm_now_ms(void)
{
    unsigned long freq, cnt;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
    return freq ? (cnt / (freq / 1000UL)) : 0;
}
static int  a_streq(const char *x, const char *y)
{ while (*x && *y) { if (*x != *y) return 0; x++; y++; } return *x == *y; }
static unsigned vm_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int      vm_i32(const unsigned char *p)
{ return (int)(p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24)); }

/* ===== staged upload (filled by /actor/loadvm chunks) ===================== */
#define AVM_STAGE_MAX (12*1024*1024)   /* room for a solid turntable (many frames) */
static unsigned char avm_stage[AVM_STAGE_MAX];
static int           avm_stage_len;
/* On-screen upload progress (drawn by the WM via avm_draw_loadbar). */
static volatile int  avm_ld_active;    /* a binary upload is in flight */
static volatile int  avm_ld_cur;       /* bytes received so far */
static volatile int  avm_ld_total;     /* expected total (from ?total=) */
/* Resumable launch-from-SD read state (declared here so avm_draw_loadbar can
 * stream it; filled by avm_run_listed). */
static int          pend_active;
static fat32_t      pend_fs;
static unsigned int pend_cur, pend_got, pend_want;
int                 avm_loadrun(int len);   /* forward decl */
/* SD-read diagnostic (shown on screen when a launch read fails). */
static int          avm_dg_on;
static unsigned int avm_dg_clu, avm_dg_lba, avm_dg_got;
void avm_stage_reset(void) { avm_stage_len = 0; avm_ld_cur = 0; avm_ld_active = 1; }
int  avm_stage_put(int off, const unsigned char *b, int n)
{
    if (off < 0 || n < 0 || off + n > AVM_STAGE_MAX) return -1;
    for (int i = 0; i < n; i++) avm_stage[off + i] = b[i];
    if (off + n > avm_stage_len) avm_stage_len = off + n;
    return 0;
}
/* Called per chunk from the HTTP handler so the bar tracks the upload. */
void avm_load_progress(int cur, int total)
{
    avm_ld_active = 1; avm_ld_cur = cur;
    if (total > 0) avm_ld_total = total;
}

/* ===== parsed module ===================================================== */
#define VM_STR_MAX     512
#define VM_STRBUF_MAX  16384
#define VM_MAX_CLASSES 16
#define VM_MAX_METHODS 320     /* a turntable packs ~14 chunk methods x many frames into one class */
typedef struct { unsigned short name; unsigned char n_params; int code_len, code_off; } vmmeth_t;
typedef struct { unsigned short name, n_fields, n_methods; vmmeth_t m[VM_MAX_METHODS]; } vmclass_t;
static const char *vm_str[VM_STR_MAX];
static char        vm_strbuf[VM_STRBUF_MAX];
static int         vm_n_str;
static vmclass_t   vm_class[VM_MAX_CLASSES];
static int         vm_n_class;
static unsigned char *vm_mod;          /* = avm_stage */
static int            vm_mod_len;

/* ===== objects (actor instances) + FIFO message queue ==================== */
#define VM_MAXOBJ 64
#define VM_MAXF   24
typedef struct { int used, cls; long f[VM_MAXF]; } vmobj_t;
static vmobj_t vm_obj[VM_MAXOBJ];
static int     vm_nobj;

#define VM_Q 512
typedef struct { int self, sender, na; const char *method; long a[8]; } vmmsg_t;
static vmmsg_t vm_q[VM_Q];
static int     vm_qh, vm_qt;
static void vm_enqueue(int self, int recv, const char *method, int na, long *a)
{
    int nx = (vm_qt + 1) % VM_Q;
    if (nx == vm_qh) return;             /* queue full: drop */
    vm_q[vm_qt].self = recv; vm_q[vm_qt].sender = self;
    vm_q[vm_qt].method = method; vm_q[vm_qt].na = na;
    for (int i = 0; i < na && i < 8; i++) vm_q[vm_qt].a[i] = a[i];
    vm_qt = nx;
}

static int vm_spawn(int cls)
{
    if (cls < 0 || cls >= vm_n_class) return -1;
    for (int i = 0; i < VM_MAXOBJ; i++) if (!vm_obj[i].used) {
        vm_obj[i].used = 1; vm_obj[i].cls = cls;
        for (int f = 0; f < VM_MAXF; f++) vm_obj[i].f[f] = 0;
        if (i >= vm_nobj) vm_nobj = i + 1;
        return i;
    }
    return -1;
}

/* ===== draw buffers + off-screen raster (the "Blender" display) ========== */
#define BW 760
#define BH 620
static unsigned int g_buf[BW * BH];      /* off-screen ARGB raster          */
static int          g_buf_ready;
#define VLINE_MAX 512
#define VTRI_MAX  24000
typedef struct { short x1,y1,x2,y2; unsigned char col; } avm_line_t;
typedef struct { short x1,y1,x2,y2,x3,y3; unsigned char col; } avm_tri_t;
static avm_line_t v_line[VLINE_MAX];
static int v_line_n;
static avm_tri_t  v_tri[VTRI_MAX];
static int v_tri_n;
#define G_BG 0xFF06100AU                          /* content background */

/* ---- SMP: rasterise across the 4 Cortex-A72 cores -------------------------
 * The frame is split into horizontal scanline bands; each core fills ONE band
 * (clears it + draws every triangle/line clipped to its rows).  Bands are
 * disjoint pixel rows, so painter order is preserved and there are no races. */
extern int  smp_cores_online(void);
typedef long (*avm_range_fn)(long lo, long hi, int core);
extern long smp_parallel_sum(avm_range_fn fn, long n, int ncores);

/* ---- turntable frame cache + playback control (Start/Stop/Pause/arrows) ----
 * As the actor plays its first full turn we cache each frame's tri/line lists;
 * once a cycle completes the WM-side controls drive which frame shows. */
#define FC_MAX 24
static avm_tri_t  fc_tri[FC_MAX][VTRI_MAX];
static int fc_ntri[FC_MAX];
static avm_line_t fc_line[FC_MAX][VLINE_MAX];
static int fc_nline[FC_MAX];
static int  ctl_cached;        /* a full turn is cached -> control mode */
static int  ctl_nframes;       /* frames in the turntable */
static int  ctl_prevf = -1;    /* last frame index seen (cycle detect) */
static int  ctl_play  = 1;     /* auto-advance on/off */
static int  ctl_idx;           /* frame shown in control mode */
static int  ctl_dir   = 1;     /* +1 / -1 rotation direction */
static int  ctl_speed = 240;   /* ms per frame (from WAIT) */
static unsigned long ctl_last; /* pacing timestamp */

static unsigned int pal16(int c)
{
    static const unsigned int p[16] = {
        0xFF000000U,0xFF3060FFU,0xFF30D040U,0xFF30D0D0U,
        0xFFE03030U,0xFFE040E0U,0xFFE0E040U,0xFFF0F0F0U,
        0xFF808080U,0xFF80A0FFU,0xFF80FF80U,0xFF80FFFFU,
        0xFFFF8080U,0xFFFF80FFU,0xFFFFFF80U,0xFFFFFFFFU };
    return p[c & 15];
}

/* scanline-fill a triangle into g_buf, clipped to scanline band [ylo,yhi). */
static void buf_tri_band(int x0,int y0,int x1,int y1,int x2,int y2,unsigned int col,int ylo,int yhi)
{
    int s;
    if (y1<y0){s=x0;x0=x1;x1=s;s=y0;y0=y1;y1=s;}
    if (y2<y0){s=x0;x0=x2;x2=s;s=y0;y0=y2;y2=s;}
    if (y2<y1){s=x1;x1=x2;x2=s;s=y1;y1=y2;y2=s;}
    if (y2==y0) return;
    int ya = y0 < ylo ? ylo : y0;
    int yb = y2 >= yhi ? yhi - 1 : y2;
    for (int yy=ya; yy<=yb; yy++) {
        if (yy<0 || yy>=BH) continue;
        int xa = x0 + (int)((long)(x2-x0)*(yy-y0)/(y2-y0)), xb;
        if (yy<y1 && y1!=y0) xb = x0 + (int)((long)(x1-x0)*(yy-y0)/(y1-y0));
        else if (y2!=y1)     xb = x1 + (int)((long)(x2-x1)*(yy-y1)/(y2-y1));
        else                 xb = x1;
        if (xa>xb){s=xa;xa=xb;xb=s;}
        if (xa<0) xa=0; if (xb>=BW) xb=BW-1;
        unsigned int *row = g_buf + yy*BW;
        for (int xx=xa; xx<=xb; xx++) row[xx]=col;
    }
}
static void buf_line_band(int x0,int y0,int x1,int y1,unsigned int col,int ylo,int yhi)
{
    int dx=a_abs(x1-x0), sx=x0<x1?1:-1, dy=-a_abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy, e2;
    for (;;) {
        if (x0>=0&&x0<BW&&y0>=ylo&&y0<yhi&&y0<BH) g_buf[y0*BW+x0]=col;
        if (x0==x1&&y0==y1) break;
        e2=2*err; if (e2>=dy){err+=dy;x0+=sx;} if (e2<=dx){err+=dx;y0+=sy;}
    }
}
/* One SMP worker: clear + draw the rows [lo,hi) (called on each core). */
static long avm_render_band(long lo, long hi, int core)
{
    (void)core;
    for (int yy=(int)lo; yy<(int)hi; yy++) {
        unsigned int *row = g_buf + yy*BW;
        for (int x=0;x<BW;x++) row[x]=G_BG;
    }
    for (int i=0;i<v_tri_n;i++)
        buf_tri_band(v_tri[i].x1,v_tri[i].y1,v_tri[i].x2,v_tri[i].y2,v_tri[i].x3,v_tri[i].y3,
                     pal16(v_tri[i].col),(int)lo,(int)hi);
    for (int i=0;i<v_line_n;i++)
        buf_line_band(v_line[i].x1,v_line[i].y1,v_line[i].x2,v_line[i].y2,
                      pal16(v_line[i].col),(int)lo,(int)hi);
    return 0;
}
/* rasterise the accumulated frame into g_buf, fanned out over all cores. */
static void avm_render(void)
{
    int nc = smp_cores_online();
    if (nc < 1) nc = 1;
    smp_parallel_sum(avm_render_band, BH, nc);
    g_buf_ready = 1;
}

/* Load a cached turntable frame into the live buffers and rasterise it. */
static void ctl_show(int idx)
{
    if (idx < 0 || idx >= ctl_nframes) return;
    v_tri_n = fc_ntri[idx];
    for (int i=0;i<v_tri_n;i++) v_tri[i] = fc_tri[idx][i];
    v_line_n = fc_nline[idx];
    for (int i=0;i<v_line_n;i++) v_line[i] = fc_line[idx][i];
    avm_render();
}

/* opcode-facing draw API (called from dispatch) */
static void vm_cls(void)  { v_line_n = 0; v_tri_n = 0; }
static void vm_line(int x1,int y1,int x2,int y2,int col)
{ if (v_line_n<VLINE_MAX){ v_line[v_line_n].x1=x1;v_line[v_line_n].y1=y1;v_line[v_line_n].x2=x2;v_line[v_line_n].y2=y2;v_line[v_line_n].col=(unsigned char)col;v_line_n++; } }
static void vm_tri(int x1,int y1,int x2,int y2,int x3,int y3,int col)
{ if (v_tri_n<VTRI_MAX){ v_tri[v_tri_n].x1=x1;v_tri[v_tri_n].y1=y1;v_tri[v_tri_n].x2=x2;v_tri[v_tri_n].y2=y2;v_tri[v_tri_n].x3=x3;v_tri[v_tri_n].y3=y3;v_tri[v_tri_n].col=(unsigned char)col;v_tri_n++; } }

/* ===== VM graphics window (toolbar + Blender display) ==================== */
#define AVM_TB 22                                   /* toolbar height */
static const char *avm_btn[5] = { "Play", "Pause", "Stop", " <", " >" };
static window_t vmgfx_win;
static int      vmgfx_added;
static void vmgfx_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    avm_tick();                             /* advance / control the VM */
    /* toolbar: Play / Pause / Stop / prev / next */
    int tx = self->x + 2, ty = self->y + WM_TITLEBAR_H + 2, bw = BW / 5;
    for (int i = 0; i < 5; i++) {
        int bx = tx + i * bw;
        unsigned int bg = 0xFF1F3550U;
        if ((i == 0 && ctl_play) || (i == 1 && !ctl_play)) bg = 0xFF1F6E2EU;  /* active */
        fill_rect(bx, ty, bw - 2, AVM_TB, bg);
        draw_rect(bx, ty, bw - 2, AVM_TB, 0xFF6080A0U);
        draw_string_at(bx + 10, ty + (AVM_TB - 8) / 2, avm_btn[i], 0xFFE8F0F8U, bg);
    }
    if (g_buf_ready)
        video_blit(self->x + 2, self->y + WM_TITLEBAR_H + 2 + AVM_TB + 1, BW, BH, g_buf, BW);
}
/* Toolbar click -> playback control.  (lx,ly) are window-local. */
static void vmgfx_click(window_t *self, int lx, int ly)
{
    (void)self;
    int tytop = WM_TITLEBAR_H + 2, tybot = tytop + AVM_TB;
    if (ly < tytop || ly >= tybot) return;
    int bx = lx - 2; if (bx < 0) return;
    int bw = BW / 5; if (bw < 1) bw = 1;
    int i = bx / bw;
    if (i >= 0 && i <= 4) avm_ctl(i);
}
/* Arrow / space keys when the VM window is active: rotate / play / pause.
 * The caller (main.c) only invokes this when wm_kbd_target() is this window. */
window_t *avm_window(void) { return &vmgfx_win; }
void avm_key(char c)
{
    /* ESC [ A/B/C/D arrows arrive as a 3-byte sequence; main.c forwards the
     * final letter.  Also accept WASD + space as direct controls. */
    switch (c) {
        case 'D': case 'a': avm_ctl(3); break;      /* left  = prev */
        case 'C': case 'd': avm_ctl(4); break;      /* right = next */
        case 'A': case 'w': avm_ctl(0); break;      /* up    = play */
        case 'B': case 's': avm_ctl(1); break;      /* down  = pause */
        case ' ': avm_ctl(ctl_play ? 1 : 0); break; /* space = toggle */
        default: break;
    }
}

/* ===== on-screen load bar (drawn by the WM each frame during an upload) === */
void avm_draw_loadbar(int sw, int sh)
{
    /* Stream a launch-from-SD read a few clusters per frame so the bar animates.
     * Robust: re-check pend_active each step, and on a read error finish with
     * whatever we have (or abort) so a bad sector can never freeze the bar. */
    if (pend_active) {
        unsigned int spc = pend_fs.sectors_per_cluster ? pend_fs.sectors_per_cluster : 1;
        int budget = 16;                         /* clusters this frame */
        unsigned char sec[512];
        while (pend_active && budget-- > 0) {
            if (pend_got >= pend_want || !(pend_cur >= 2 && pend_cur < 0x0FFFFFF8u)) {
                int n = (int)pend_got; pend_active = 0;
                avm_loadrun(n);                  /* done: clears avm_ld_active, spawns */
                break;
            }
            unsigned long base = fat32_cluster_lba(&pend_fs, pend_cur);
            int err = 0;
            for (unsigned int s = 0; s < spc && pend_got < pend_want; s++) {
                int rok = 0;
                for (int t = 0; t < 4 && !rok; t++) if (pend_fs.rd(base + s, sec) == 0) rok = 1;
                if (!rok) { avm_dg_on = 1; avm_dg_clu = pend_cur; avm_dg_lba = (unsigned int)(base + s);
                            avm_dg_got = pend_got; err = 1; break; }   /* read error: record it */
                unsigned int chunk = pend_want - pend_got;
                if (chunk > 512) chunk = 512;
                for (unsigned int i = 0; i < chunk; i++) avm_stage[pend_got + i] = sec[i];
                pend_got += chunk;
            }
            if (err) {                           /* read failed: don't freeze */
                pend_active = 0;
                if (pend_got >= pend_want) avm_loadrun((int)pend_got);
                else avm_ld_active = 0;          /* abort: hide the bar */
                break;
            }
            pend_cur = fat32_next_cluster_of(&pend_fs, pend_cur);
            avm_ld_cur = (int)pend_got;
        }
    }
    /* SD-read failure diagnostic (stays until the next launch). */
    if (avm_dg_on) {
        int dx = (sw - 460) / 2, dy = sh - 90;
        fill_rect(dx - 4, dy - 4, 468, 30, 0xFF200A0AU);
        draw_rect(dx - 4, dy - 4, 468, 30, 0xFFFF5050U);
        char m[64]; int p = 0;
        const char *t = "SD read FAIL clu=";
        for (int i = 0; t[i]; i++) m[p++] = t[i];
        unsigned int v = avm_dg_clu; char tmp[12]; int tn = 0;
        if (!v) tmp[tn++] = '0'; while (v) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn) m[p++] = tmp[--tn];
        const char *t2 = " lba="; for (int i = 0; t2[i]; i++) m[p++] = t2[i];
        v = avm_dg_lba; tn = 0; if (!v) tmp[tn++]='0'; while (v){ tmp[tn++]=(char)('0'+v%10); v/=10; }
        while (tn) m[p++] = tmp[--tn];
        const char *t3 = " got="; for (int i = 0; t3[i]; i++) m[p++] = t3[i];
        v = avm_dg_got; tn = 0; if (!v) tmp[tn++]='0'; while (v){ tmp[tn++]=(char)('0'+v%10); v/=10; }
        while (tn) m[p++] = tmp[--tn];
        /* EMMC INTERRUPT bits at the failure (bit16=CTO 17=CCRC 20=DTO 21=DCRC). */
        { extern unsigned int sd_last_int(void); unsigned int iv = sd_last_int();
          const char *t4 = " int=0x"; for (int i=0;t4[i];i++) m[p++]=t4[i];
          for (int sh4=28; sh4>=0; sh4-=4){ int nib=(iv>>sh4)&0xF; m[p++]=(char)(nib<10?'0'+nib:'A'+nib-10); } }
        m[p] = 0;
        draw_string_at(dx, dy + 2, m, 0xFFFFD0D0U, 0xFF200A0AU);
        return;
    }
    if (!avm_ld_active) return;
    int bw = 420, bh = 30, bx = (sw - bw) / 2, by = sh - 90;
    int pct = (avm_ld_total > 0) ? (int)((long)avm_ld_cur * 100 / avm_ld_total) : 0;
    if (pct > 100) pct = 100;
    fill_rect(bx - 4, by - 20, bw + 8, bh + 28, 0xFF0A1420U);     /* panel       */
    draw_rect(bx - 4, by - 20, bw + 8, bh + 28, 0xFF40A0FFU);     /* blue border */
    draw_string_at(bx, by - 16, "Loading .avm actor...", 0xFFE8F0F8U, 0xFF0A1420U);
    draw_rect(bx, by, bw, bh, 0xFF6080A0U);                       /* track       */
    fill_rect(bx + 1, by + 1, (bw - 2) * pct / 100, bh - 2, 0xFF30D040U); /* fill */
    /* percent text */
    char p[5]; int n = 0;
    if (pct >= 100) { p[n++]='1';p[n++]='0';p[n++]='0'; }
    else if (pct >= 10) { p[n++]=(char)('0'+pct/10); p[n++]=(char)('0'+pct%10); }
    else p[n++]=(char)('0'+pct);
    p[n++]='%'; p[n]=0;
    draw_string_at(bx + bw/2 - 12, by + (bh-8)/2, p, 0xFF000000U, 0xFF30D040U);
}

/* ===== module loader (AVM1) ============================================== */
static int avm_load(unsigned char *buf, int len)
{
    if (len < 6) return -1;
    if (buf[0]!='A'||buf[1]!='V'||buf[2]!='M'||buf[3]!='1') return -1;
    vm_mod = buf; vm_mod_len = len;
    const unsigned char *p = buf + 4, *end = buf + len;
    int ns = vm_u16(p); p += 2; vm_n_str = 0; int sb = 0, i;
    for (i = 0; i < ns && i < VM_STR_MAX; i++) {
        if (p + 2 > end) return -1;
        int l = vm_u16(p); p += 2;
        if (p + l > end || sb + l + 1 > VM_STRBUF_MAX) return -1;
        vm_str[i] = &vm_strbuf[sb];
        for (int k=0;k<l;k++) vm_strbuf[sb+k]=p[k];
        vm_strbuf[sb+l]=0; sb += l+1; p += l;
    }
    vm_n_str = i;
    if (p + 2 > end) return -1;
    int nc = vm_u16(p); p += 2; vm_n_class = 0;
    int c, mi;
    for (c = 0; c < nc && c < VM_MAX_CLASSES; c++) {
        if (p + 6 > end) return -1;
        vmclass_t *cl = &vm_class[c];
        cl->name = vm_u16(p); p+=2; cl->n_fields = vm_u16(p); p+=2; cl->n_methods = vm_u16(p); p+=2;
        for (mi = 0; mi < cl->n_methods && mi < VM_MAX_METHODS; mi++) {
            if (p + 5 > end) return -1;
            cl->m[mi].name = vm_u16(p); p+=2; cl->m[mi].n_params = *p++;
            cl->m[mi].code_len = vm_u16(p); p+=2; cl->m[mi].code_off = (int)(p - buf);
            if (p + cl->m[mi].code_len > end) return -1;
            p += cl->m[mi].code_len;
        }
        vm_n_class++;
    }
    return vm_n_class > 0 ? 0 : -1;
}

/* ===== bytecode dispatch (one message) ================================== */
#define VM_VSTACK 128
static void avm_dispatch(int self, int sender, const char *method, long *args, int n_args)
{
    if (self < 0 || self >= VM_MAXOBJ || !vm_obj[self].used) return;
    vmclass_t *cl = &vm_class[vm_obj[self].cls];
    vmmeth_t *mt = 0;
    for (int i = 0; i < cl->n_methods; i++) {
        unsigned short nm = cl->m[i].name;
        if (nm < vm_n_str && a_streq(vm_str[nm], method)) { mt = &cl->m[i]; break; }
    }
    if (!mt) return;
    const unsigned char *code = vm_mod + mt->code_off; int clen = mt->code_len;
    long stk[VM_VSTACK]; int sp = 0, pc = 0; long guard = 0;
#define VPUSH(v) do { if (sp < VM_VSTACK) stk[sp++] = (long)(v); } while (0)
#define VPOP()   (sp > 0 ? stk[--sp] : 0)
    while (pc < clen) {
        if (++guard > 5000000L) break;
        unsigned char op = code[pc++];
        switch (op) {
        case 0x01: VPUSH(vm_i32(code+pc)); pc+=4; break;                       /* PUSHI */
        case 0x02: { int f=code[pc++]; VPUSH(f<VM_MAXF?vm_obj[self].f[f]:0); } break;  /* GETFIELD */
        case 0x03: { int f=code[pc++]; long v=VPOP(); if (f<VM_MAXF) vm_obj[self].f[f]=v; } break; /* SETFIELD */
        case 0x04: { int a=code[pc++]; VPUSH(a<n_args?args[a]:0); } break;     /* GETARG */
        case 0x05: VPUSH(self); break;                                          /* SELF */
        case 0x06: VPUSH(sender); break;                                        /* SENDER */
        case 0x07: {                                                            /* WAIT = frame boundary */
            vm_wait_ms = (int)VPOP();
            if (vm_wait_ms > 0) ctl_speed = vm_wait_ms;
            if (!ctl_cached) {
                int cf = (self >= 0 && self < VM_MAXOBJ) ? (int)vm_obj[self].f[0] : 0;
                if (cf >= 0 && cf < FC_MAX) {                /* cache this frame */
                    fc_ntri[cf] = v_tri_n;
                    for (int i=0;i<v_tri_n;i++) fc_tri[cf][i] = v_tri[i];
                    fc_nline[cf] = v_line_n;
                    for (int i=0;i<v_line_n;i++) fc_line[cf][i] = v_line[i];
                    if (cf + 1 > ctl_nframes) ctl_nframes = cf + 1;
                }
                if (cf < ctl_prevf) ctl_cached = 1;          /* frame index wrapped -> full turn cached */
                ctl_prevf = cf;
                avm_render();                                /* show the live frame while caching */
            }
            vm_frame_done = 1;
        } break;
        case 0x08: { long v = sp>0?stk[sp-1]:0; VPUSH(v); } break;             /* DUP */
        case 0x10: { long b=VPOP(),a=VPOP(); VPUSH(a+b); } break;
        case 0x11: { long b=VPOP(),a=VPOP(); VPUSH(a-b); } break;
        case 0x12: { long b=VPOP(),a=VPOP(); VPUSH(a*b); } break;
        case 0x13: { long b=VPOP(),a=VPOP(); VPUSH(b?a/b:0); } break;
        case 0x14: { long b=VPOP(),a=VPOP(); VPUSH(b?a%b:0); } break;
        case 0x20: { long b=VPOP(),a=VPOP(); VPUSH(a<b); } break;
        case 0x21: { long b=VPOP(),a=VPOP(); VPUSH(a<=b); } break;
        case 0x22: { long b=VPOP(),a=VPOP(); VPUSH(a>b); } break;
        case 0x23: { long b=VPOP(),a=VPOP(); VPUSH(a>=b); } break;
        case 0x24: { long b=VPOP(),a=VPOP(); VPUSH(a==b); } break;
        case 0x25: { long b=VPOP(),a=VPOP(); VPUSH(a!=b); } break;
        case 0x30: pc = vm_u16(code+pc); break;                                 /* JMP */
        case 0x31: { int t=vm_u16(code+pc); pc+=2; if (VPOP()==0) pc=t; } break;/* JMPZ */
        case 0x40: { int mn=vm_u16(code+pc); pc+=2; int na=code[pc++], i;       /* SEND */
                     long va[8]; if (na>8) na=8;
                     for (i=na-1;i>=0;i--) va[i]=VPOP();
                     int recv=(int)VPOP();
                     if (mn>=0 && mn<vm_n_str) vm_enqueue(self, recv, vm_str[mn], na, va); } break;
        case 0x41: { int ci=vm_u16(code+pc); pc+=2; VPUSH(vm_spawn(ci)); } break; /* SPAWN */
        case 0x42: { (void)VPOP(); } break;                                     /* PRINT (ignored) */
        case 0x43: pc = clen; break;                                            /* RET */
        case 0x44: { int fi=vm_u16(code+pc); pc+=2; int na=code[pc++]; (void)fi; /* PRINTF (ignored) */
                     for (int i=0;i<na;i++) VPOP(); } break;
        case 0x45: { long col=VPOP(),y2=VPOP(),x2=VPOP(),y1=VPOP(),x1=VPOP();   /* LINE */
                     vm_line((int)x1,(int)y1,(int)x2,(int)y2,(int)col); } break;
        case 0x46: vm_cls(); break;                                             /* CLS */
        case 0x47: { long col=VPOP(),y3=VPOP(),x3=VPOP(),y2=VPOP(),x2=VPOP(),y1=VPOP(),x1=VPOP(); /* TRI */
                     vm_tri((int)x1,(int)y1,(int)x2,(int)y2,(int)x3,(int)y3,(int)col); } break;
        default: pc = clen; break;
        }
    }
#undef VPUSH
#undef VPOP
}

/* ===== cooperative tick: advance the VM by ONE frame ===================== */
/* Called once per WM redraw (from vmgfx_draw).  Processes queued messages until
 * a WAIT (frame boundary) or the queue empties, then returns control to the WM
 * so the screen + network keep running.  No thread, no busy-wait. */
static int           avm_active;
static unsigned long  avm_last_ms;
static void avm_tick(void)
{
    if (!avm_active) return;

    /* Control mode: a full turn is cached -> the WM controls play/pause/step
     * instead of running the actor.  Auto-advance honours ctl_speed (the WAIT
     * ms); when paused the current cached frame just stays on screen. */
    if (ctl_cached) {
        if (ctl_play && ctl_nframes > 0) {
            unsigned long now = avm_now_ms();
            if (now - ctl_last >= (unsigned long)ctl_speed) {
                ctl_idx = (ctl_idx + ctl_dir + ctl_nframes) % ctl_nframes;
                ctl_show(ctl_idx);
                ctl_last = now;
            }
        }
        return;
    }

    /* Caching phase: pace by the WAIT ms, advancing one actor frame at a time. */
    if (g_buf_ready && vm_wait_ms > 0) {
        unsigned long now = avm_now_ms();
        if (now - avm_last_ms < (unsigned long)vm_wait_ms) return;
        avm_last_ms = now;
    }
    vm_frame_done = 0;
    long guard = 0;
    while (vm_qh != vm_qt && !vm_frame_done && ++guard < 2000000L) {
        vmmsg_t m = vm_q[vm_qh]; vm_qh = (vm_qh + 1) % VM_Q;
        avm_dispatch(m.self, m.sender, m.method, m.a, m.na);
    }
}

/* ---- playback controls (toolbar buttons + arrow keys) ---- */
void avm_ctl(int cmd)   /* 0 play, 1 pause, 2 stop, 3 prev, 4 next */
{
    if (!ctl_cached) return;
    switch (cmd) {
        case 0: ctl_play = 1; break;                                  /* Start  */
        case 1: ctl_play = 0; break;                                  /* Pause  */
        case 2: ctl_play = 0; ctl_idx = 0; ctl_show(0); break;        /* Stop   */
        case 3: ctl_play = 0; ctl_idx = (ctl_idx - 1 + ctl_nframes) % ctl_nframes; ctl_show(ctl_idx); break; /* < */
        case 4: ctl_play = 0; ctl_idx = (ctl_idx + 1) % ctl_nframes;  ctl_show(ctl_idx); break;             /* > */
        default: break;
    }
}

/* ===== public entry: load the staged module and run it =================== */
int avm_loadrun(int len)
{
    if (len <= 0 || len > AVM_STAGE_MAX) return -1;
    /* reset runtime */
    avm_ld_active = 0;                             /* upload finished: hide the bar */
    vm_qh = vm_qt = 0; vm_nobj = 0;
    for (int i=0;i<VM_MAXOBJ;i++) vm_obj[i].used = 0;
    v_line_n = 0; v_tri_n = 0; g_buf_ready = 0;
    ctl_cached = 0; ctl_nframes = 0; ctl_prevf = -1;    /* fresh turntable: re-cache */
    ctl_play = 1; ctl_idx = 0;
    if (avm_load(avm_stage, len) != 0) return -1;

    if (!vmgfx_added) {                          /* open the VM graphics window */
        vmgfx_win.x = 120; vmgfx_win.y = 50;
        vmgfx_win.width = BW + 4; vmgfx_win.height = BH + WM_TITLEBAR_H + AVM_TB + 5;
        const char *t = "VM graphics (AVM)";
        int k; for (k=0;k<WM_TITLE_MAX && t[k];k++) vmgfx_win.title[k]=t[k]; vmgfx_win.title[k]=0;
        vmgfx_win.font_scale = 1;
        vmgfx_win.chrome_color = 0xFFAACCEEU; vmgfx_win.title_bg = 0xFF102030U;
        vmgfx_win.title_fg = 0xFFFFFFFFU; vmgfx_win.content_bg = 0xFF06100AU;
        vmgfx_win.draw_content = vmgfx_draw;
        vmgfx_win.on_click     = vmgfx_click;
        wm_add(&vmgfx_win);
        vmgfx_added = 1;
    }

    int id = vm_spawn(0);                         /* class 0 = synthetic __boot */
    if (id < 0) return -1;
    vm_enqueue(-1, id, "tick", 0, 0);
    avm_active = 1;                                /* vmgfx_draw will tick it */
    return id;
}

/* ===== AIPL actor browser ================================================
 * Right-click menu "Run AIPL actor" opens a window listing every .avm on the
 * microSD (M:) and USB /sd (S:); clicking a row reads that whole file off the
 * card (multi-cluster) into the stage and runs it. */
#define AVM_LIST_MAX 40
static struct { char name[16]; char vol; } avm_list[AVM_LIST_MAX];
static int      avm_list_n;
static window_t avm_listwin;
static int      avm_listwin_added;

static int avm_mount_vol(char vol, fat32_t *fs)
{
    extern int usbmsd_ready(void);
    extern int usbmsd_read_block(unsigned long, void *);
    extern int usbmsd_write_block(unsigned long, const void *);
    if (vol == 'S') { if (!usbmsd_ready()) return -1;
                      return fat32_mount_dev(fs, usbmsd_read_block, usbmsd_write_block); }
    return fat32_mount(fs);                         /* 'M' = microSD (EMMC2) */
}
static void avm_list_visit(const char *name, int is_dir, unsigned long size,
                           unsigned int fc, int depth, void *ctx)
{
    (void)size; (void)fc; (void)depth;
    if (is_dir) return;
    int l = 0; while (name[l]) l++;
    if (l < 5) return;
    const char *e = name + l - 4;                   /* match ".AVM" (any case) */
    if (!(e[0]=='.' && (e[1]=='A'||e[1]=='a') && (e[2]=='V'||e[2]=='v') && (e[3]=='M'||e[3]=='m'))) return;
    if (avm_list_n < AVM_LIST_MAX) {
        int i = 0; for (; name[i] && i < 15; i++) avm_list[avm_list_n].name[i] = name[i];
        avm_list[avm_list_n].name[i] = 0;
        avm_list[avm_list_n].vol = *(char *)ctx;
        avm_list_n++;
    }
}
static void avm_scan(void)
{
    avm_list_n = 0;
    fat32_t fs; char v;
    v = 'M'; if (avm_mount_vol('M', &fs) == 0) fat32_walk_dir(&fs, fs.root_cluster, 0, avm_list_visit, &v);
    v = 'S'; if (avm_mount_vol('S', &fs) == 0) fat32_walk_dir(&fs, fs.root_cluster, 0, avm_list_visit, &v);
}
/* Resumable launch read: avm_draw_loadbar() streams the file off the card a few
 * clusters per frame (animating the load bar), then runs it. */
static void avm_run_listed(int idx)
{
    if (idx < 0 || idx >= avm_list_n) return;
    if (avm_mount_vol(avm_list[idx].vol, &pend_fs) != 0) return;
    unsigned int first = 0, size = 0;
    if (fat32_open(&pend_fs, avm_list[idx].name, &first, &size) != 0 || first < 2) return;
    pend_cur = first; pend_got = 0;
    pend_want = (size < AVM_STAGE_MAX) ? size : AVM_STAGE_MAX;
    pend_active = 1; avm_dg_on = 0;                 /* clear old diagnostic */
    avm_ld_active = 1; avm_ld_cur = 0; avm_ld_total = (int)pend_want;
}

/* Right-click menu "MAKINA": run SOLID.AVM straight from a copy EMBEDDED in the
 * kernel image — no SD/USB read at all, so it's 100% reliable regardless of the
 * flaky EMMC / storage reads. */
extern unsigned char makina_solid_avm[];
extern unsigned int  makina_solid_avm_len;
void avm_run_makina(void)
{
    pend_active = 0; avm_dg_on = 0;                 /* no streaming read needed */
    unsigned int n = makina_solid_avm_len;
    if (n == 0 || n > AVM_STAGE_MAX) return;
    for (unsigned int i = 0; i < n; i++) avm_stage[i] = makina_solid_avm[i];
    avm_loadrun((int)n);                            /* parse + spawn from RAM */
}
static void avm_listwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    extern void wm_cursor_desktop(int *, int *);
    int cx = 0, cy = 0; wm_cursor_desktop(&cx, &cy);
    int x = self->x + 6, y = self->y + WM_TITLEBAR_H + 6;
    draw_string_at(x, y, "AIPL actors - click to run:", 0xFFFFE0A0U, self->content_bg);
    y += 16;
    int hov = -1;
    if (cx >= self->x && cx < self->x + self->width) hov = (cy - y) / 12;
    for (int i = 0; i < avm_list_n; i++) {
        char line[24]; int p = 0;
        line[p++] = avm_list[i].vol; line[p++] = ':'; line[p++] = ' ';
        for (int k = 0; avm_list[i].name[k] && p < 22; k++) line[p++] = avm_list[i].name[k];
        line[p] = 0;
        unsigned int bg = (i == hov) ? 0xFFFFE0A0U : self->content_bg;   /* invert on hover */
        unsigned int fg = (i == hov) ? 0xFF101010U : 0xFFCFE8FFU;
        if (i == hov) fill_rect(x - 2, y + i * 12 - 1, self->width - 8, 12, bg);
        draw_string_at(x, y + i * 12, line, fg, bg);
    }
    if (avm_list_n == 0)
        draw_string_at(x, y, "(no .avm on microSD / sd)", 0xFF888888U, self->content_bg);
}
static void avm_listwin_click(window_t *self, int lx, int ly)
{
    (void)self; (void)lx;
    int top = WM_TITLEBAR_H + 6 + 16;
    int row = (ly - top) / 12;
    if (row >= 0 && row < avm_list_n) avm_run_listed(row);
}
/* Save the just-staged .avm to the microSD as `name` (8.3).  Called from the
 * loadvm handler when ?save=NAME is given.  Returns 0 ok, <0 on error. */
static int avm_mount_vol(char vol, fat32_t *fs);   /* fwd decl (defined below) */
int avm_save(const char *name, int len)
{
    if (len <= 0 || len > AVM_STAGE_MAX) return -1;
    fat32_t fs;
    /* Prefer the reliable USB drive (/sd); fall back to microSD (EMMC2). */
    if (avm_mount_vol('S', &fs) == 0)
        return fat32_write_file_full(&fs, name, avm_stage, (unsigned int)len);
    if (fat32_mount(&fs) == 0)
        return fat32_write_file_full(&fs, name, avm_stage, (unsigned int)len);
    return -1;
}

void avm_open_list(void)            /* called from the right-click menu */
{
    extern void wm_show(window_t *);
    avm_scan();
    if (!avm_listwin_added) {
        avm_listwin.x = 690; avm_listwin.y = 80;
        avm_listwin.width = 320; avm_listwin.height = 320;
        const char *t = "AIPL actors"; int k;
        for (k = 0; k < WM_TITLE_MAX && t[k]; k++) avm_listwin.title[k] = t[k];
        avm_listwin.title[k] = 0;
        avm_listwin.font_scale = 1;
        avm_listwin.chrome_color = 0xFFFFB060U; avm_listwin.title_bg = 0xFF704020U;
        avm_listwin.title_fg = 0xFFFFFFFFU; avm_listwin.content_bg = 0xFF14100AU;
        avm_listwin.draw_content = avm_listwin_draw;
        avm_listwin.on_click     = avm_listwin_click;
        avm_listwin_added = 1;
    }
    wm_show(&avm_listwin);
}
