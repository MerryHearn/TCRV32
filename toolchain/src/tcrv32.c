/* tcrv32.c —— tcrv32.h 的实现（屏幕控制 / 文本控制台 / 简易 printf / 字符串）
 *
 * 依赖 support.c 里的：putch / putstr / putdec / puthex / memcpy / memset
 * 编译：与 support.c、crt0.S 一起链（见 tools/build_m2.py）
 */
#include "tcrv32.h"
#include <stdarg.h>

/* ============================ 屏幕设置 ============================ */

void scr_set(int opt, uint32_t val) {
    /* 两步：先写"选项号寄存器"，再写"值"（写值那一拍触发屏幕的"写入设置"） */
    *(volatile uint32_t *)TCRV32_SCR_OPT = (uint32_t)opt;
    *(volatile uint32_t *)TCRV32_SCR_VAL = val;
}

void scr_set2(int id, int opt, uint32_t val) {
    if (id < 0 || id >= TCRV32_SCR_N) return;
    *(volatile uint32_t *)TCRV32_SCR_OPT_OF(id) = (uint32_t)opt;
    *(volatile uint32_t *)TCRV32_SCR_VAL_OF(id) = val;   /* 写值即触发"写入设置" */
}

void scr_set_offset(uint32_t off) { scr_set(SCR_OPT_OFFSET, off); }
void scr_set_mode(int mode)       { scr_set(SCR_OPT_MODE, (uint32_t)mode); }
void scr_set_offset2(int id, uint32_t off) { scr_set2(id, SCR_OPT_OFFSET, off); }
void scr_set_mode2(int id, int mode)       { scr_set2(id, SCR_OPT_MODE, (uint32_t)mode); }

void scr_init(uint32_t vram_off, int mode) {
    scr_set_offset(vram_off);      /* 先设偏移量 */
    scr_set_mode(mode);            /* 再设模式（顺序无所谓，但都要做） */
}

void scr_set_res2(int id, int setting) { scr_set2(id, SCR_OPT_RES, (uint32_t)setting); }
void scr_set_res(int setting)          { scr_set(SCR_OPT_RES, (uint32_t)setting); }

void scr_init2(int id, uint32_t vram_off, int mode) {
    scr_set_offset2(id, vram_off);
    scr_set_mode2(id, mode);
}

/* 像素模式常用：偏移 → 分辨率 → 模式（顺序无关，但都要做） */
void scr_init_pixel(int id, uint32_t fb_off, int setting) {
    scr_set_offset2(id, fb_off);
    scr_set_res2(id, setting);
    scr_set_mode2(id, SCR_PIXEL8);
}

/* ==================== 图形（M3 骨架） ==================== */

uint32_t gfx_base   = TCRV32_FB0;
uint32_t gfx_w      = SCR_COLS;     /* 默认按 ASCII8 的 96×40 */
uint32_t gfx_h      = SCR_ROWS;
uint32_t gfx_stride = SCR_COLS;
uint32_t gfx_bpp    = 1;

void gfx_target(uint32_t base, uint32_t w, uint32_t h, uint32_t bpp, uint32_t stride) {
    gfx_base = base;
    gfx_w = w ? w : SCR_COLS;
    gfx_h = h ? h : SCR_ROWS;
    gfx_bpp = bpp ? bpp : 1;
    gfx_stride = stride ? stride : gfx_w * gfx_bpp;
}

void gfx_target_res(uint32_t base, int setting, uint32_t bpp) {
    gfx_target(base, (uint32_t)SCR_RES_W(setting), (uint32_t)SCR_RES_H(setting), bpp, 0);
}

void gfx_hline(int x, int y, int w, uint32_t color) {
    if (y < 0 || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (w <= 0) return;
    volatile uint8_t *p = gfx_row(y);
    if (gfx_bpp == 1) {
        uint8_t c = (uint8_t)color;
        for (int i = 0; i < w; i++) p[x + i] = c;         /* 直接下标，无调用 */
    } else {
        for (int i = 0; i < w; i++) gfx_putpixel(x + i, y, color);
    }
}
void gfx_vline(int x, int y, int h, uint32_t color) {
    for (int i = 0; i < h; i++) gfx_putpixel(x, y + i, color);
}
void gfx_fill(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0) continue;
        if (yy >= (int)gfx_h) break;
        gfx_hline(x, yy, w, color);
    }
}
void gfx_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}
void gfx_clear(uint32_t color) {
    gfx_fill(0, 0, (int)gfx_w, (int)gfx_h, color);
}

/* Bresenham 直线 */
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* 中点圆 */
void gfx_circle(int cx, int cy, int r, uint32_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_putpixel(cx + x, cy + y, color);
        gfx_putpixel(cx + y, cy + x, color);
        gfx_putpixel(cx - y, cy + x, color);
        gfx_putpixel(cx - x, cy + y, color);
        gfx_putpixel(cx - x, cy - y, color);
        gfx_putpixel(cx - y, cy - x, color);
        gfx_putpixel(cx + y, cy - x, color);
        gfx_putpixel(cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* ============================ 键盘 ============================ */
/* 取事件的规范（2026-11 定，来自真机实测）：
 *   ① **一次只读一条**：读 `EVENT`（+0x08）就是消费，绝不"多读几条看看/筛选"；
 *   ② **用 `非空`（`STAT`）做门控**：它只要队列里有未读事件就常亮（电平），
 *      所以 STAT 为 0 时连读都不读，为 1 时读一次即可；
 *   ③ **两次读取之间不能长时间空闲**：真机上"读一次然后停很久"会让事件越积越多、
 *      表现为随机丢/延迟。调用方要把取键摊进整个循环（`snake.c` 把它插进绘制的每一小段），
 *      而不是一帧只在开头取一次。
 */

int kbd_waiting(void) {
    return (int)(*(volatile uint32_t *)TCRV32_KBD_STAT & 1u);
}

int kbd_get_event(int *code, int *down) {
    if (!kbd_waiting()) return 0;                          /* ② 空就什么都不做 */

    uint32_t v = *(volatile uint32_t *)TCRV32_KBD_EVENT;    /* ① 只读一次：取走 + 弹出 */
    if ((v & 0x1FFu) == 0) return 0;                        /* 这一拍没取到：不再补读 */

    if (code) *code = (int)(v & 0xFFu);
    if (down) *down = (int)((v >> 8) & 1u);
    return 1;
}

int kbd_drain(int *codes, int max) {
    int n = 0;
    if (!codes || max <= 0) return 0;
    while (n < max) {
        int code, down;
        if (!kbd_get_event(&code, &down)) break;
        codes[n++] = down ? code : (code | 0x100);  /* 弹起用 bit8 标记 */
    }
    return n;
}

/* ======================= 文本控制台（屏幕） ======================= */

static int g_cx, g_cy;             /* 软件光标：列、行 */

int  con_getx(void) { return g_cx; }
int  con_gety(void) { return g_cy; }

void con_gotoxy(int row, int col) {
    if (row < 0) row = 0;
    if (row >= SCR_ROWS) row = SCR_ROWS - 1;
    if (col < 0) col = 0;
    if (col >= SCR_COLS) col = SCR_COLS - 1;
    g_cy = row; g_cx = col;
}

void con_clear(int ch) {
    volatile uint8_t *p = (volatile uint8_t *)TCRV32_VRAM_BASE;
    for (uint32_t i = 0; i < SCR_CELLS; i++) p[i] = (uint8_t)ch;
    g_cx = g_cy = 0;
}

static void con_newline(void) {
    g_cx = 0;
    if (++g_cy >= SCR_ROWS) {       /* 滚屏：整屏上移一行 */
        volatile uint8_t *p = (volatile uint8_t *)TCRV32_VRAM_BASE;
        for (uint32_t i = 0; i < SCR_CELLS - SCR_COLS; i++)
            p[i] = p[i + SCR_COLS];
        for (uint32_t i = SCR_CELLS - SCR_COLS; i < SCR_CELLS; i++) p[i] = ' ';
        g_cy = SCR_ROWS - 1;
    }
}

void con_putch(int ch) {
    if (ch == '\n') { con_newline(); return; }
    if (ch == '\r') { g_cx = 0; return; }
    scr_put(g_cy, g_cx, ch);
    if (++g_cx >= SCR_COLS) con_newline();
}

void con_puts(const char *s) { while (*s) con_putch(*s++); }

void con_putdec(long v) {
    char b[24]; int i = 0; int neg = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    while (i) con_putch(b[--i]);
}

void con_puthex(uint32_t v) {
    static const char *H = "0123456789abcdef";
    for (int s = 28; s >= 0; s -= 4) con_putch(H[(v >> s) & 0xF]);
}

/* ========================= 简易 printf ========================= */
/* 支持 %d %u %x %X %s %c %% ；宽度/浮点不支持 */

typedef void (*emit_fn)(int ch, void *ctx);

static void emit_out(int ch, void *ctx) { (void)ctx; putch((char)ch); }
static void emit_con(int ch, void *ctx) { (void)ctx; con_putch(ch); }

static void vfmt(emit_fn emit, void *ctx, const char *fmt, va_list ap) {
    char b[24];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(*p, ctx); continue; }
        p++;
        switch (*p) {
        case 'd': {
            long v = va_arg(ap, long);
            int i = 0, neg = 0; unsigned long u;
            if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
            if (!u) b[i++] = '0';
            while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
            if (neg) b[i++] = '-';
            while (i) emit(b[--i], ctx);
            break;
        }
        case 'u': {
            unsigned long u = va_arg(ap, unsigned long);
            int i = 0;
            if (!u) b[i++] = '0';
            while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
            while (i) emit(b[--i], ctx);
            break;
        }
        case 'x': case 'X': {
            static const char *H = "0123456789abcdef";
            static const char *HU = "0123456789ABCDEF";
            uint32_t v = va_arg(ap, uint32_t);
            int started = 0;
            for (int s = 28; s >= 0; s -= 4) {
                int nib = (v >> s) & 0xF;
                if (nib || started || s == 0) { started = 1; emit((*p == 'x' ? H : HU)[nib], ctx); }
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) emit(*s++, ctx);
            break;
        }
        case 'c': emit(va_arg(ap, int), ctx); break;
        case '%': emit('%', ctx); break;
        case 0:   return;
        default:  emit('%', ctx); emit(*p, ctx); break;
        }
    }
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfmt(emit_out, 0, fmt, ap); va_end(ap);
    return 0;
}

int con_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfmt(emit_con, 0, fmt, ap); va_end(ap);
    return 0;
}

/* ========================= 字符串 ========================= */

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}
