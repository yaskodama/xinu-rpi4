/* device/wifi/wifi.c — BCM43455/CYW43455 SDIO bring-up for Raspberry Pi 4
 * (BCM2711, peripheral base 0xFE000000).
 *
 * Ported from the proven xinu-raz (Pi 3) driver.  Pi 4 differences vs Pi 3:
 *   - peripheral base 0x3F000000 -> 0xFE000000 (so EMMC 0xFE300000, GPIO
 *     0xFE200000, clock-mgr 0xFE101000, system timer 0xFE003000)
 *   - mailbox goes through the kernel's mbox_call() (correct Pi 4 send;
 *     D-cache is OFF so the static buffer is coherent with the VideoCore)
 *   - no Xinu threads/semaphores here: this is the cooperative single-core
 *     kernel, so M0 is a straight-line sequence (no SDIO mutex needed)
 *
 * MILESTONE 0: power the chip (WL_REG_ON via the firmware GPIO expander),
 * bring up the Arasan SDHCI host, enumerate SDIO (CMD0/5/3/7), enable the
 * F1 backplane, and read the silicon chip-id (expect 0x4345) + core EROM.
 * Firmware download / scan / join are later milestones.
 *
 * The WiFi SDIO host is the Arasan "emmc" controller; on the Pi the SD card
 * is on a *different* controller (emmc2 @ 0xFE340000), so driving this one
 * for WiFi does not disturb the boot SD.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern int  mbox_call(volatile unsigned int *buf);   /* device/mbox/mbox.c */

/* ------------------------------------------------------------------ *
 *  trace log (HTTP /wifi-probe dumps this) + mini formatter          *
 * ------------------------------------------------------------------ */
static char wifi_tbuf[8000];
static int  wifi_tn;

static void wlog_putc(char c)
{
    if (wifi_tn < (int)sizeof(wifi_tbuf) - 1) wifi_tbuf[wifi_tn++] = c;
    uart_putc(c);
}
static void wlog_str(const char *s) { while (*s) wlog_putc(*s++); }
static void wlog_u(u32 v, int base, int width, char pad)
{
    char t[16]; int n = 0;
    const char *dig = "0123456789abcdef";
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = dig[v % base]; v /= base; }
    while (n < width) t[n++] = pad;
    while (n--) wlog_putc(t[n]);
}
/* supports %d %u %x %08x %02x %s %c %% */
static void wifi_log(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { wlog_putc(*fmt); continue; }
        fmt++;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); fmt++; }
        switch (*fmt) {
        case 'd': { int v = __builtin_va_arg(ap, int);
                    if (v < 0) { wlog_putc('-'); v = -v; }
                    wlog_u((u32)v, 10, width, pad); } break;
        case 'u': wlog_u(__builtin_va_arg(ap, u32), 10, width, pad); break;
        case 'x': wlog_u(__builtin_va_arg(ap, u32), 16, width, pad); break;
        case 's': wlog_str(__builtin_va_arg(ap, const char *)); break;
        case 'c': wlog_putc((char)__builtin_va_arg(ap, int)); break;
        case '%': wlog_putc('%'); break;
        default:  wlog_putc('%'); wlog_putc(*fmt); break;
        }
    }
    __builtin_va_end(ap);
}

const char *wifi_trace(void) { return wifi_tbuf; }

/* ------------------------------------------------------------------ *
 *  MMIO bases (BCM2711, peripheral base 0xFE000000)                  *
 * ------------------------------------------------------------------ */
#define WIFI_GPIO_BASE     0xFE200000UL
#define WIFI_EMMC_BASE     0xFE300000UL   /* Arasan SDHCI = WiFi SDIO host */
#define WIFI_CM_BASE       0xFE101000UL
#define SYSTIMER_CLO       (*(volatile u32 *)(0xFE003000UL + 0x04))   /* 1 MHz */

#define GPFSEL3            (*(volatile u32 *)(WIFI_GPIO_BASE + 0x0C))
#define GPFSEL4            (*(volatile u32 *)(WIFI_GPIO_BASE + 0x10))
#define GPFSEL5            (*(volatile u32 *)(WIFI_GPIO_BASE + 0x14))
#define GPPUD              (*(volatile u32 *)(WIFI_GPIO_BASE + 0x94))
#define GPPUDCLK1          (*(volatile u32 *)(WIFI_GPIO_BASE + 0x9C))
#define CM_GP2CTL          (*(volatile u32 *)(WIFI_CM_BASE + 0x80))
#define CM_GP2DIV          (*(volatile u32 *)(WIFI_CM_BASE + 0x84))

#define EMMC_ARG2          (*(volatile u32 *)(WIFI_EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile u32 *)(WIFI_EMMC_BASE + 0x04))
#define EMMC_ARG1          (*(volatile u32 *)(WIFI_EMMC_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile u32 *)(WIFI_EMMC_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile u32 *)(WIFI_EMMC_BASE + 0x10))
#define EMMC_DATA          (*(volatile u32 *)(WIFI_EMMC_BASE + 0x20))
#define EMMC_STATUS        (*(volatile u32 *)(WIFI_EMMC_BASE + 0x24))
#define EMMC_CONTROL0      (*(volatile u32 *)(WIFI_EMMC_BASE + 0x28))
#define EMMC_CONTROL1      (*(volatile u32 *)(WIFI_EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT     (*(volatile u32 *)(WIFI_EMMC_BASE + 0x30))
#define EMMC_IRPT_MASK     (*(volatile u32 *)(WIFI_EMMC_BASE + 0x34))
#define EMMC_IRPT_EN       (*(volatile u32 *)(WIFI_EMMC_BASE + 0x38))
#define EMMC_SLOTISR_VER   (*(volatile u32 *)(WIFI_EMMC_BASE + 0xFC))

#define SR_CMD_INHIBIT     (1u << 0)
#define C1_CLK_INTLEN      (1u << 0)
#define C1_CLK_STABLE      (1u << 1)
#define C1_CLK_EN          (1u << 2)
#define C1_TOUNIT_MAX      (0xeu << 16)
#define C1_SRST_HC         (1u << 24)
#define INT_CMD_DONE       (1u << 0)
#define INT_DATA_DONE      (1u << 1)
#define INT_WRITE_RDY      (1u << 4)
#define INT_READ_RDY       (1u << 5)
#define INT_ERR            0xFFFF8000u
#define CMD_RSPNS_NONE     (0u << 16)
#define CMD_RSPNS_48       (2u << 16)
#define CMD_RSPNS_48BUSY   (3u << 16)
#define CMD_CRCCHK_EN      (1u << 19)
#define CMD_IXCHK_EN       (1u << 20)
#define CMD_ISDATA         (1u << 21)
#define TM_DAT_DIR_CH      (1u << 4)

#define SD_CMD0_GO_IDLE         0
#define SD_CMD3_SEND_RCA        3
#define SD_CMD5_IO_OP_COND      5
#define SD_CMD7_SELECT          7
#define SD_CMD52_IO_RW_DIRECT   52
#define SD_CMD53_IO_RW_EXT      53
#define SDIO_CCCR_IOEx          0x02
#define SDIO_CCCR_IORx          0x03

#define SBSDIO_FUNC1_SBADDRLOW  0x1000A
#define SBSDIO_SB_OFT_ADDR_MASK 0x07FFF
#define SBSDIO_SB_ACCESS_2_4B   0x08000
#define SBSDIO_SBWINDOW_MASK    0xFFFF8000u
#define SI_ENUM_BASE            0x18000000u
#define CID_ID_MASK             0x0000FFFFu
#define BCM43455_CHIP_ID        0x4345
#define WIFI_GPIO_WL_ON_FW      129        /* expander pin 1 (WL_ON) + base 128 */
#define RPI_FW_EMMC_CLK_ID      1

#define SB_WSIZE   0x8000u
#define SB_32BIT   0x8000u
#define SB_ENUMBASE 0x18000000u
#define CORE_ARMCR4 0x83E
#define CORE_ARMCM3 0x82A
#define CORE_ARM7   0x825
#define CORE_CHIPCOMMON 0x800
#define CORE_SOCRAM 0x80E
#define CORE_SDIODEV 0x829
#define CORE_D11    0x812
#define SB_XFER     2048

/* ------------------------------------------------------------------ *
 *  delays                                                            *
 * ------------------------------------------------------------------ */
static void wifi_udelay(volatile unsigned long n) { while (n--) asm volatile("nop"); }
static void wifi_delay_us(u32 us) { u32 s = SYSTIMER_CLO; while ((SYSTIMER_CLO - s) < us) {} }
static void wifi_msleep(u32 ms) { while (ms--) wifi_delay_us(1000); }
/* Arasan erratum: space out register writes at the slow init clock. */
static void ew(volatile u32 *reg, u32 val) { *reg = val; wifi_delay_us(12); }

/* ------------------------------------------------------------------ *
 *  VideoCore property mailbox via the kernel mbox_call()             *
 * ------------------------------------------------------------------ */
static volatile u32 mbox_buf[36] __attribute__((aligned(16)));

static int wifi_mbox_prop(u32 tag, const u32 *in, int n_in,
                          u32 *out, int n_out, int vbuf_bytes)
{
    volatile u32 *b = mbox_buf;
    int i, hdr = 5;
    b[0] = (hdr + (vbuf_bytes / 4) + 1) * 4;
    b[1] = 0;
    b[2] = tag;
    b[3] = vbuf_bytes;
    b[4] = 0;
    for (i = 0; i < n_in; i++) b[hdr + i] = in[i];
    for (i = n_in; i < vbuf_bytes / 4; i++) b[hdr + i] = 0;
    b[hdr + vbuf_bytes / 4] = 0;
    if (mbox_call(b) != 0) return -1;
    for (i = 0; i < n_out; i++) out[i] = b[hdr + i];
    return (b[1] == 0x80000000u) ? 0 : -1;
}
static int wifi_fw_set_gpio(u32 gpio, u32 state)
{ u32 in[2] = { gpio, state }; return wifi_mbox_prop(0x00038041, in, 2, 0, 0, 8); }
static int wifi_fw_get_gpio(u32 gpio, u32 *state)
{ u32 in[2] = { gpio, 0 }, out[2] = {0,0}; int rc = wifi_mbox_prop(0x00030041, in, 1, out, 2, 8);
  if (state) *state = out[1]; return rc; }
static int wifi_fw_set_clock_state(u32 id, u32 on)
{ u32 in[2] = { id, on & 1 }, out[2] = {0,0}; return wifi_mbox_prop(0x00038001, in, 2, out, 2, 8); }
static u32 wifi_fw_get_clock_rate(u32 id)
{ u32 in[1] = { id }, out[2] = {0,0}; if (wifi_mbox_prop(0x00030002, in, 1, out, 2, 8) != 0) return 0; return out[1]; }

/* ------------------------------------------------------------------ *
 *  GPIO routing: WIFI_CLK (GPIO43 ALT0) + SDIO bus (GPIO34-39 ALT3)  *
 * ------------------------------------------------------------------ */
static void wifi_clk_setup(void)
{
    u32 sel = GPFSEL4;
    int alt43 = (sel >> 9) & 7;
    if (alt43 != 4) { sel &= ~(7u << 9); sel |= (4u << 9); GPFSEL4 = sel;
        wifi_log("[wifi] WIFI_CLK: set GPIO43 -> ALT0 (was %d)\r\n", alt43); }
    wifi_log("[wifi] WIFI_CLK: GP2CTL=0x%08x GP2DIV=0x%08x\r\n", CM_GP2CTL, CM_GP2DIV);
}
static void wifi_gpio_sdio(void)
{
    u32 sel; int pin;
    /* free the Arasan from the SD-card pins (GPIO48-53 -> ALT0) */
    sel = GPFSEL4; sel &= ~((7u<<24)|(7u<<27)); sel |= ((4u<<24)|(4u<<27)); GPFSEL4 = sel;
    sel = GPFSEL5; sel &= ~((7u<<0)|(7u<<3)|(7u<<6)|(7u<<9));
                   sel |=  ((4u<<0)|(4u<<3)|(4u<<6)|(4u<<9)); GPFSEL5 = sel;
    /* GPIO34-39 -> ALT3 (=7) */
    sel = GPFSEL3;
    for (pin = 34; pin <= 39; pin++) { int sh = 3*(pin-30); sel &= ~(7u<<sh); sel |= (7u<<sh); }
    GPFSEL3 = sel;
    /* pull-up CMD/DAT (GPIO35-39), no-pull CLK (GPIO34) */
    GPPUD = 2; wifi_udelay(300);
    GPPUDCLK1 = (1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7); wifi_udelay(300);
    GPPUD = 0; GPPUDCLK1 = 0;
    GPPUD = 0; wifi_udelay(300); GPPUDCLK1 = (1u<<2); wifi_udelay(300);
    GPPUD = 0; GPPUDCLK1 = 0;
}

/* ------------------------------------------------------------------ *
 *  Arasan SDHCI host bring-up                                        *
 * ------------------------------------------------------------------ */
static u32 emmc_divisor(u32 base_hz, u32 target_hz)
{
    u32 d = 1; if (!target_hz) target_hz = 400000;
    while ((base_hz / (2*d)) > target_hz && d < 1023) d++;
    return d;
}
static int emmc_reset_clock(u32 divisor)
{
    u32 c1; int t;
    ew(&EMMC_CONTROL1, EMMC_CONTROL1 | C1_SRST_HC);
    for (t = 0; t < 100000; t++) { if (!(EMMC_CONTROL1 & C1_SRST_HC)) break; wifi_delay_us(100); }
    if (EMMC_CONTROL1 & C1_SRST_HC) { wifi_log("[wifi]   SRST_HC stuck\r\n"); return -1; }
    ew(&EMMC_CONTROL0, (EMMC_CONTROL0 & ~0xF00u) | (1u<<8) | (7u<<9));
    c1 = EMMC_CONTROL1; c1 &= ~0xFFE0u; c1 |= C1_CLK_INTLEN | C1_TOUNIT_MAX;
    c1 |= (divisor & 0xFF) << 8; c1 |= ((divisor >> 8) & 0x3) << 6;
    ew(&EMMC_CONTROL1, c1);
    for (t = 0; t < 100000; t++) { if (EMMC_CONTROL1 & C1_CLK_STABLE) break; wifi_delay_us(100); }
    if (!(EMMC_CONTROL1 & C1_CLK_STABLE)) { wifi_log("[wifi]   clk not stable\r\n"); return -1; }
    ew(&EMMC_CONTROL1, EMMC_CONTROL1 | C1_CLK_EN); wifi_delay_us(2000);
    return 0;
}
static int emmc_host_init(void)
{
    u32 ver, base, div;
    wifi_fw_set_clock_state(RPI_FW_EMMC_CLK_ID, 1);
    base = wifi_fw_get_clock_rate(RPI_FW_EMMC_CLK_ID);
    wifi_log("[wifi] EMMC base clock = %u Hz\r\n", base);
    if (!base) { base = 250000000u; wifi_log("[wifi]   (assuming %u)\r\n", base); }
    ver = EMMC_SLOTISR_VER;
    wifi_log("[wifi] SDHCI ver=0x%02x (SLOTISR=0x%08x)\r\n", (ver>>16)&0xFF, ver);
    div = emmc_divisor(base, 400000);
    wifi_log("[wifi] init clk divisor=%u (~%u Hz)\r\n", div, base/(2*div));
    if (emmc_reset_clock(div) != 0) return -1;
    ew(&EMMC_IRPT_EN, 0xFFFFFFFFu); ew(&EMMC_IRPT_MASK, 0xFFFFFFFFu); ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);
    wifi_log("[wifi] SDHCI ready: CTL0=0x%08x CTL1=0x%08x STAT=0x%08x\r\n",
             EMMC_CONTROL0, EMMC_CONTROL1, EMMC_STATUS);
    return 0;
}
static int emmc_cmd(u32 idx, u32 arg, u32 flags, u32 *resp, int rd4, u32 *data)
{
    u32 cmd, intr, t;
    for (t = 0; t < 1000000; t++) if (!(EMMC_STATUS & SR_CMD_INHIBIT)) break;
    ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);
    if (rd4) ew(&EMMC_BLKSIZECNT, (1u<<16) | 4);
    ew(&EMMC_ARG1, arg);
    cmd = (idx << 24) | flags;
    if (rd4) cmd |= CMD_ISDATA | TM_DAT_DIR_CH;
    ew(&EMMC_CMDTM, cmd);
    for (t = 0; t < 1000000; t++) {
        intr = EMMC_INTERRUPT;
        if (intr & INT_ERR) { wifi_log("[wifi]   cmd%d err 0x%08x\r\n", idx, intr); return -1; }
        if (intr & INT_CMD_DONE) { EMMC_INTERRUPT = INT_CMD_DONE; break; }
    }
    if (t >= 1000000) { wifi_log("[wifi]   cmd%d timeout\r\n", idx); return -1; }
    if (resp) *resp = EMMC_RESP0;
    if (rd4) {
        for (t = 0; t < 1000000; t++) { intr = EMMC_INTERRUPT;
            if (intr & INT_ERR) return -1;
            if (intr & INT_READ_RDY) { EMMC_INTERRUPT = INT_READ_RDY; break; } }
        if (t >= 1000000) return -1;
        if (data) *data = EMMC_DATA;
        for (t = 0; t < 1000000; t++) if (EMMC_INTERRUPT & INT_DATA_DONE) { EMMC_INTERRUPT = INT_DATA_DONE; break; }
    }
    return 0;
}
static int sdio_cmd52(int func, u32 addr, int write, u32 val)
{
    u32 arg, resp;
    arg = ((u32)(write&1)<<31) | ((u32)(func&7)<<28) | (write?(1u<<27):0) |
          ((addr & 0x1FFFF) << 9) | (val & 0xFF);
    if (emmc_cmd(SD_CMD52_IO_RW_DIRECT, arg, CMD_RSPNS_48|CMD_CRCCHK_EN|CMD_IXCHK_EN, &resp, 0, 0) != 0)
        return -1;
    return (int)(resp & 0xFF);
}
static int sdio_cmd53_read32(u32 f1off, u32 *out)
{
    u32 arg = (0u<<31)|(1u<<28)|(0u<<27)|(1u<<26)|((f1off&0x1FFFF)<<9)|(4u&0x1FF);
    return emmc_cmd(SD_CMD53_IO_RW_EXT, arg, CMD_RSPNS_48|CMD_CRCCHK_EN|CMD_IXCHK_EN, 0, 1, out);
}
static int sdio_set_window(u32 addr)
{
    u32 v = (addr & SBSDIO_SBWINDOW_MASK) >> 8; int i;
    for (i = 0; i < 3; i++, v >>= 8)
        if (sdio_cmd52(1, SBSDIO_FUNC1_SBADDRLOW + i, 1, v & 0xFF) < 0) return -1;
    return 0;
}
static int wifi_backplane_read32(u32 addr, u32 *out)
{
    if (sdio_set_window(addr) != 0) return -1;
    return sdio_cmd53_read32((addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B, out);
}
static int wifi_cmd53_pio(int write, int fn, u32 off, u8 *buf, int len, int incr)
{
    u32 arg, intr, t, total, w, words; u32 bsize = (fn==2)?512u:64u; int blkmode, blkcnt;
    if (len <= 0) return 0;
    if ((u32)len > bsize) { blkmode = 1; blkcnt = len/bsize; total = blkcnt*bsize; }
    else                  { blkmode = 0; blkcnt = len;       total = len; }
    for (t = 0; t < 1000000; t++) if (!(EMMC_STATUS & SR_CMD_INHIBIT)) break;
    ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);
    if (blkmode) ew(&EMMC_BLKSIZECNT, ((u32)blkcnt<<16)|bsize);
    else         ew(&EMMC_BLKSIZECNT, (1u<<16)|(u32)total);
    arg = ((u32)(write&1)<<31)|((u32)(fn&7)<<28)|((u32)(blkmode&1)<<27)|((u32)(incr&1)<<26)|
          ((off&0x1FFFF)<<9)|((blkmode?(u32)blkcnt:(u32)total)&0x1FF);
    ew(&EMMC_ARG1, arg);
    { u32 cmd = (SD_CMD53_IO_RW_EXT<<24)|CMD_RSPNS_48|CMD_CRCCHK_EN|CMD_IXCHK_EN|CMD_ISDATA;
      if (!write) cmd |= TM_DAT_DIR_CH;
      if (blkmode) cmd |= (1u<<5)|(1u<<1);
      ew(&EMMC_CMDTM, cmd); }
    for (t = 0; t < 1000000; t++) { intr = EMMC_INTERRUPT;
        if (intr & INT_ERR) { wifi_log("[wifi]   cmd53 err 0x%08x\r\n", intr); return -1; }
        if (intr & INT_CMD_DONE) { EMMC_INTERRUPT = INT_CMD_DONE; break; } }
    if (t >= 1000000) return -1;
    words = (total + 3) / 4;
    { u32 done = 0;
      while (done < words) {
        u32 flag = write ? INT_WRITE_RDY : INT_READ_RDY;
        u32 chunk = (total > bsize && (words-done) > (bsize/4)) ? (bsize/4) : (words-done);
        for (t = 0; t < 1000000; t++) { intr = EMMC_INTERRUPT;
            if (intr & INT_ERR) return -1;
            if (intr & flag) { EMMC_INTERRUPT = flag; break; } }
        if (t >= 1000000) return -1;
        for (w = 0; w < chunk; w++) { u32 idx = (done+w)*4;
            if (write) { u32 v = (u32)buf[idx]|((u32)buf[idx+1]<<8)|((u32)buf[idx+2]<<16)|((u32)buf[idx+3]<<24); EMMC_DATA = v; }
            else       { u32 v = EMMC_DATA; buf[idx]=v; buf[idx+1]=v>>8; buf[idx+2]=v>>16; buf[idx+3]=v>>24; } }
        done += chunk;
      } }
    for (t = 0; t < 1000000; t++) if (EMMC_INTERRUPT & INT_DATA_DONE) { EMMC_INTERRUPT = INT_DATA_DONE; break; }
    return 0;
}
static int wifi_sbmem(int write, u8 *buf, int len, u32 addr)
{
    while (len > 0) {
        u32 woff; int wrem = (int)(SB_WSIZE - (addr & (SB_WSIZE-1))); int chunk = len;
        if (chunk > SB_XFER) chunk = SB_XFER;
        if (chunk > wrem) chunk = wrem;
        if (sdio_set_window(addr) != 0) return -1;
        woff = (addr & (SB_WSIZE-1)) | SB_32BIT;
        if (wifi_cmd53_pio(write, 1, woff, buf, (chunk+3)&~3, 1) != 0) return -1;
        addr += chunk; buf += chunk; len -= chunk;
    }
    return 0;
}

/* discovered chip layout */
static u32 chip_chipcommon, chip_armctl, chip_armregs, chip_armcore;
static u32 chip_d11ctl, chip_socramctl, chip_socramregs, chip_sdregs;

static int wifi_corescan(void)
{
    static u8 erom[512]; u32 eromptr, i; int coreid = 0;
    if (wifi_backplane_read32(SB_ENUMBASE + 63*4, &eromptr) != 0) return -1;
    chip_chipcommon = chip_armctl = chip_armregs = chip_armcore = 0;
    chip_d11ctl = chip_socramctl = chip_socramregs = chip_sdregs = 0;
    if (wifi_sbmem(0, erom, sizeof(erom), eromptr) != 0) return -1;
    for (i = 0; i < sizeof(erom); i += 4) {
        u32 addr;
        switch (erom[i] & 0xF) {
        case 0xF: return 0;
        case 0x1:
            if ((erom[i+4] & 0xF) != 0x1) break;
            coreid = (erom[i+1] | (erom[i+2]<<8)) & 0xFFF; i += 4; break;
        case 0x5:
            addr = (erom[i+1]<<8)|(erom[i+2]<<16)|(erom[i+3]<<24); addr &= ~0xFFFu;
            switch (coreid) {
            case CORE_CHIPCOMMON: if ((erom[i]&0xC0)==0) chip_chipcommon = addr; break;
            case CORE_ARMCM3: case CORE_ARM7: case CORE_ARMCR4:
                chip_armcore = coreid;
                if (erom[i]&0xC0) { if (!chip_armctl) chip_armctl = addr; }
                else              { if (!chip_armregs) chip_armregs = addr; } break;
            case CORE_SOCRAM:
                if (erom[i]&0xC0) chip_socramctl = addr; else if (!chip_socramregs) chip_socramregs = addr; break;
            case CORE_SDIODEV: if ((erom[i]&0xC0)==0) chip_sdregs = addr; break;
            case CORE_D11: if (erom[i]&0xC0) chip_d11ctl = addr; break;
            }
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  MILESTONE 0 bring-up + probe                                      *
 * ------------------------------------------------------------------ */
static int wifi_ready;

int wifi_bringup(void)
{
    u32 ocr = 0, rca = 0, chipid = 0; int r, ioe = 0;
    if (wifi_ready) { wifi_log("[wifi] (already up)\r\n"); return 0; }
    wifi_log("[wifi] === Pi4 BCM43455 SDIO bring-up (M0) ===\r\n");

    { u32 f3 = GPFSEL3, f4 = GPFSEL4; int p;
      wifi_log("[wifi] fw GPFSEL3=0x%08x GPFSEL4=0x%08x\r\n", f3, f4);
      for (p = 34; p <= 39; p++) wifi_log("[wifi]   GPIO%d fsel=%d\r\n", p, (f3>>(3*(p-30)))&7);
      wifi_log("[wifi]   GPIO43 fsel=%d\r\n", (f4>>9)&7); }

    wifi_clk_setup();

    { u32 st = 0xDEAD;
      wifi_log("[wifi] WL_REG_ON cycle (fw gpio %d)...\r\n", WIFI_GPIO_WL_ON_FW);
      wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 0); wifi_msleep(50);
      if (wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 1) != 0) wifi_log("[wifi]   SET_GPIO not OK\r\n");
      wifi_msleep(150);
      if (wifi_fw_get_gpio(WIFI_GPIO_WL_ON_FW, &st) == 0) wifi_log("[wifi] WL_REG_ON readback=%u (want 1)\r\n", st);
      else wifi_log("[wifi]   GET_GPIO failed\r\n"); }

    wifi_gpio_sdio();
    wifi_log("[wifi] GPIO34-39 -> ALT3 (GPFSEL3=0x%08x)\r\n", GPFSEL3);

    if (emmc_host_init() != 0) { wifi_log("[wifi] FAILED: host init\r\n"); return -1; }

    emmc_cmd(SD_CMD0_GO_IDLE, 0, CMD_RSPNS_NONE, 0, 0, 0); wifi_msleep(2);
    r = emmc_cmd(SD_CMD5_IO_OP_COND, 0, CMD_RSPNS_48, &ocr, 0, 0);
    if (r != 0) { wifi_log("[wifi] FAILED: CMD5 (no SDIO device)\r\n"); return -1; }
    wifi_log("[wifi] CMD5 OCR=0x%08x (numfn=%d)\r\n", ocr, (ocr>>28)&7);
    { int tries; u32 voltwin = ocr & 0x00FFFF00u;
      for (tries = 0; tries < 50; tries++) {
        if (emmc_cmd(SD_CMD5_IO_OP_COND, voltwin, CMD_RSPNS_48, &ocr, 0, 0) != 0) break;
        if (ocr & 0x80000000u) break; wifi_msleep(10); } }
    if (!(ocr & 0x80000000u)) { wifi_log("[wifi] FAILED: OCR not ready 0x%08x\r\n", ocr); return -1; }
    wifi_log("[wifi] SDIO ready OCR=0x%08x\r\n", ocr);

    if (emmc_cmd(SD_CMD3_SEND_RCA, 0, CMD_RSPNS_48, &rca, 0, 0) != 0) { wifi_log("[wifi] FAILED: CMD3\r\n"); return -1; }
    rca &= 0xFFFF0000u; wifi_log("[wifi] RCA=0x%08x\r\n", rca);
    if (emmc_cmd(SD_CMD7_SELECT, rca, CMD_RSPNS_48BUSY, 0, 0, 0) != 0) { wifi_log("[wifi] FAILED: CMD7\r\n"); return -1; }

    if (sdio_cmd52(0, SDIO_CCCR_IOEx, 1, 0x02) < 0) { wifi_log("[wifi] FAILED: enable F1\r\n"); return -1; }
    { int tries; for (tries = 0; tries < 50; tries++) { ioe = sdio_cmd52(0, SDIO_CCCR_IORx, 0, 0);
        if (ioe >= 0 && (ioe & 0x02)) break; wifi_msleep(2); } }
    if (!(ioe & 0x02)) { wifi_log("[wifi] FAILED: F1 not ready (0x%02x)\r\n", ioe); return -1; }
    sdio_cmd52(0, 0x110, 1, 64); sdio_cmd52(0, 0x111, 1, 0);
    sdio_cmd52(0, 0x210, 1, 512 & 0xFF); sdio_cmd52(0, 0x211, 1, (512>>8)&0xFF);
    wifi_log("[wifi] F1 enabled, blksize set\r\n");

    if (wifi_backplane_read32(SI_ENUM_BASE, &chipid) != 0) { wifi_log("[wifi] FAILED: chip-id read\r\n"); return -1; }
    wifi_log("[wifi] chipcommon[0]=0x%08x chip-id=0x%04x rev=%d\r\n",
             chipid, chipid & CID_ID_MASK, (chipid>>16)&0xF);
    if ((chipid & CID_ID_MASK) != BCM43455_CHIP_ID) {
        wifi_log("[wifi] chip-id 0x%04x != 0x4345\r\n", chipid & CID_ID_MASK); return -1; }
    wifi_log("[wifi] *** BCM43455 (0x4345) detected over SDIO ***\r\n");

    if (wifi_corescan() != 0) { wifi_log("[wifi] FAILED: corescan\r\n"); return -1; }
    wifi_log("[wifi] cores: chipcommon=0x%x armcore=0x%x armctl=0x%x armregs=0x%x\r\n",
             chip_chipcommon, chip_armcore, chip_armctl, chip_armregs);
    wifi_log("[wifi]        d11ctl=0x%x socramctl=0x%x sdregs=0x%x\r\n",
             chip_d11ctl, chip_socramctl, chip_sdregs);
    wifi_ready = 1;
    wifi_log("[wifi] *** M0 SUCCESS: chip up, cores enumerated ***\r\n");
    return 0;
}

int wifi_probe(void) { wifi_tn = 0; return wifi_bringup(); }
