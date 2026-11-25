/* tcrv32.h — TCRV32 平台接口
 *
 * 用法：#include "tcrv32.h"，编译时把 include 目录加到 -I。
 *
 * 提供：内存地图常量、屏幕、文本控制台、串口控制台、图形、键盘、GPU、
 *       堆、内存/字符串函数、软浮点（无 libm）、sys_exit。
 * 不提供：标准库头、原子、异常、线程。
 */
#ifndef TCRV32_H
#define TCRV32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 内存地图 ============================ */
/* 内存地图：由 -DTCRV32_MAP_16M / -DTCRV32_MAP_1G 选择，未指定按 1 GiB（默认）。
 * 各段容量与栈顶见 memmap.py。 */
#if defined(TCRV32_MAP_16M)
#  define TCRV32_VRAM_BASE   0x00400000u  /* 文本 VRAM */
#  define TCRV32_FB0         0x00800000u  /* 屏 0 主缓冲 */
#  define TCRV32_FB0_BACK    0x00810000u  /* 屏 0 副缓冲（+64 KiB） */
#  define TCRV32_FB1         0x00820000u  /* 屏 1（+128 KiB） */
#  define TCRV32_FB_STRIDE   0x00010000u  /* 每屏预留 64 KiB */
#  define TCRV32_SCRATCH     0x003E0000u  /* 程序自用的固定小格子（DMEM 内） */
#else
#  define TCRV32_VRAM_BASE   0x11000000u  /* 文本 VRAM */
#  define TCRV32_FB0         0x12000000u  /* 屏 0 主缓冲 */
#  define TCRV32_FB0_BACK    0x12400000u  /* 屏 0 副缓冲（+4 MiB，够 1024×768×4） */
#  define TCRV32_FB1         0x12800000u  /* 屏 1（+8 MiB） */
#  define TCRV32_FB_STRIDE   0x00400000u  /* 每屏预留 4 MiB */
#  define TCRV32_SCRATCH     0x10000000u  /* 程序自用的固定小格子（DMEM 内，距栈顶 16 MiB） */
#endif
#define TCRV32_MMIO_BASE   0x40000000u

#define TCRV32_OUT_ADDR    0x40000000u  /* 写 = OUT（32 位整字）；读 = IN（键码） */
#define TCRV32_HALT_ADDR   0x40000010u  /* 写 = 停机 */
#define TCRV32_TIMER_ADDR  0x40000020u  /* 读 = 毫秒 */
#define TCRV32_KEY_ADDR    0x40000030u  /* 读 = 键码 */
/* 屏幕设置窗口：0x40000200 + 0x10*屏号 + 0x04*域（域 0 选项号，域 1 值） */
#define TCRV32_SCR_OPT     0x40000200u  /* 屏幕 0 选项号（兼容旧名） */
#define TCRV32_SCR_VAL     0x40000204u  /* 屏幕 0 值（兼容旧名） */
#define TCRV32_SCR_WIN     0x40000200u  /* 窗口基址 */
#define TCRV32_SCR_N       2            /* 支持的屏幕数 */
#define TCRV32_SCR_OPT_OF(id) (TCRV32_SCR_WIN + 0x10u * (uint32_t)(id))
#define TCRV32_SCR_VAL_OF(id) (TCRV32_SCR_WIN + 0x10u * (uint32_t)(id) + 4u)

/* 帧缓冲：FB0 / FB0_BACK 是屏 0 的双缓冲，FB1 是屏 1；
 * 每块预留 TCRV32_FB_STRIDE 字节。 */

/* ============================ 屏幕 ============================ */
#define SCR_ASCII8       0            /* 每字节 1 字符，96×40 */
#define SCR_ASCII32      1            /* 4 字节：字符/前景/背景 */
#define SCR_PIXEL8       2            /* 每字节 1 像素 RRRGGGBB */
#define SCR_PIXEL32      3            /* 4 字节：R/G/B */

#define SCR_COLS         96
#define SCR_ROWS         40
#define SCR_CELLS        (SCR_COLS * SCR_ROWS)

/* 屏幕选项号：0 模式，1 数据偏移，2 分辨率（像素模式：W=4*(s+1)、H=3*(s+1)）。
 * 字符模式固定 96×40，不受 2 号选项影响。 */
#define SCR_OPT_MODE     0
#define SCR_OPT_OFFSET   1
#define SCR_OPT_RES      2

/* 像素模式分辨率：由 setting 算宽高 */
#define SCR_RES_W(s)     (4 * ((int)(s) + 1))
#define SCR_RES_H(s)     (3 * ((int)(s) + 1))
#define SCR_RES_MAX      255             /* 1024×768 */
/* 常用档位 */
#define SCR_SET_64x48    15              /* 4*16 × 3*16 */
#define SCR_SET_128x96   31
#define SCR_SET_160x120  39
#define SCR_SET_256x192  63              /* M3 主用 */
#define SCR_SET_320x240  79
#define SCR_SET_640x480  159
#define SCR_SET_1024x768 255

/* 先写选项号、再写值；写值那一拍生效。scr_set 作用于屏 0。 */
void scr_set(int opt, uint32_t val);
void scr_set2(int id, int opt, uint32_t val);

/* 一次性配好：数据偏移 + 模式 */
void scr_init(uint32_t vram_off, int mode);
void scr_init2(int id, uint32_t vram_off, int mode);
void scr_set_offset(uint32_t off);
void scr_set_offset2(int id, uint32_t off);
void scr_set_mode(int mode);
void scr_set_mode2(int id, int mode);
/* 像素模式的分辨率（2 号选项）；setting 见 SCR_SET_* */
void scr_set_res2(int id, int setting);
void scr_set_res(int setting);

/* 按"屏幕号 + 模式 + 分辨率 + 偏移"一次配好（像素模式最常用） */
void scr_init_pixel(int id, uint32_t fb_off, int setting);

/* ============ 图形 ============
 * 字符模式：96×40 格，ASCII8 每格 1 字节，ASCII32 每格 4 字节（字符/前景/背景）。
 * 像素模式：W = 4*(setting+1)，H = 3*(setting+1)，4:3；stride = W × bpp，无行填充。
 * 颜色：Pixel8 与 ASCII32 的颜色字节是 RRRGGGBB；Pixel32 每像素 4 字节，顺序 R、G、B。
 * 选项：0 模式、1 偏移、2 分辨率（字符模式下为前景色）、3 背景色、4 字体（仅 ASCII8）。
 * 翻页只改 1 号选项，不拷贝像素。
 */
extern uint32_t gfx_base;      /* 当前绘制目标的基址 */
extern uint32_t gfx_w;         /* 宽度（像素） */
extern uint32_t gfx_h;         /* 高度（像素） */
extern uint32_t gfx_stride;    /* 每行字节数 = gfx_w × gfx_bpp */
extern uint32_t gfx_bpp;       /* 每像素字节数 */

/* 设定绘制目标。stride 传 0 表示"等于 w*bpp" */
void gfx_target(uint32_t base, uint32_t w, uint32_t h, uint32_t bpp, uint32_t stride);
/* 按 setting 直接算好几何量 */
void gfx_target_res(uint32_t base, int setting, uint32_t bpp);
/* 内联写像素：1 次乘法 + 1 次写 */
static inline void gfx_putpixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0) return;
    volatile uint8_t *p = (volatile uint8_t *)gfx_base
                        + (uint32_t)y * gfx_stride + (uint32_t)x * gfx_bpp;
    if (gfx_bpp == 1) {
        *p = (uint8_t)color;
    } else if (gfx_bpp == 2) {
        p[0] = (uint8_t)color; p[1] = (uint8_t)(color >> 8);
    } else {
        p[0] = (uint8_t)color; p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16); p[3] = (uint8_t)(color >> 24);
    }
}
/* 取某一行首地址（批量写入时用它 + 直接下标，最省） */
static inline volatile uint8_t *gfx_row(int y) {
    return (volatile uint8_t *)gfx_base + (uint32_t)y * gfx_stride;
}
void gfx_hline(int x, int y, int w, uint32_t color);
void gfx_vline(int x, int y, int h, uint32_t color);
void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_circle(int cx, int cy, int r, uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_clear(uint32_t color);

/* 按行列直接读写字符（行 0..39，列 0..95） */
static inline void scr_put(int row, int col, int ch) {
    *(volatile uint8_t *)(TCRV32_VRAM_BASE + (uint32_t)row * SCR_COLS + (uint32_t)col)
        = (uint8_t)ch;
}
static inline int scr_get(int row, int col) {
    return *(volatile uint8_t *)(TCRV32_VRAM_BASE + (uint32_t)row * SCR_COLS + (uint32_t)col);
}

/* ==================== 文本控制台（屏幕字符矩阵） ==================== */
void con_clear(int ch);            /* 用 ch 填满整屏并把光标归零 */
void con_gotoxy(int row, int col);
int  con_getx(void);
int  con_gety(void);
void con_putch(int ch);            /* 处理 '\n'（下一行行首）与 '\r'（行首） */
void con_puts(const char *s);
void con_putdec(long v);
void con_puthex(uint32_t v);
int  con_printf(const char *fmt, ...);

/* ==================== 键盘（0x40000400） ====================
 * 硬件是事件队列：一次按键产生两条事件（按下 down=1、弹起 down=0），长按不重复。
 *   +0x00 PEEK  只读不取：bit[8] 按下位，bit[7:0] 键码
 *   +0x04 STAT  bit0 = 队列非空
 *   +0x08 EVENT 读 = 同拍取走并弹出；写 = 只弹出
 * 取事件必须用 EVENT（同拍读弹）；轮询只看 STAT，读 EVENT 就会消费掉一条。
 * 键码为字符模式实测值：字母数字同 ASCII，特殊键用游戏固定码（方向键 0x01–0x04、
 * 回车 0x0A、Esc 0x0E）。
 */
#define TCRV32_KBD_BASE    0x40000400u
#define TCRV32_KBD_PEEK    (TCRV32_KBD_BASE + 0x00u)
#define TCRV32_KBD_STAT    (TCRV32_KBD_BASE + 0x04u)
#define TCRV32_KBD_EVENT   (TCRV32_KBD_BASE + 0x08u)   /* 读 = 取走并弹出；写 = 只弹出 */
#define TCRV32_KBD_POP     TCRV32_KBD_EVENT            /* 旧名，等价 */

#define TCRV32_KEY_UP      0x01u   /* 方向键（字符模式固定码） */
#define TCRV32_KEY_RIGHT   0x02u
#define TCRV32_KEY_DOWN    0x03u
#define TCRV32_KEY_LEFT    0x04u
#define TCRV32_KEY_ENTER   0x0Au   /* LF（早期口述值） */
#define TCRV32_KEY_CR      0x0Du   /* CR：另一种可能的回车码，程序里一并接受 */
#define TCRV32_KEY_ESC     0x0Eu   /* 真机按键确认就是这个值 */
#define TCRV32_KEY_LSHIFT  0x10u
#define TCRV32_KEY_RSHIFT  0x14u
#define TCRV32_KEY_SPACE   0x20u

/* 退出键同时接受 q 与 Esc：Esc 可能被游戏界面截获，用 q 兜底。0x1B 不是 Esc。 */
#define TCRV32_IS_QUIT(c)  ((c) == 'q' || (c) == 'Q' || (c) == 0x0Eu)
#define TCRV32_IS_ESC(c)   ((c) == 0x0Eu)

/* 功能键一律用这组宏判断，不要写单值比较。 */
#define TCRV32_IS_ENTER(c) ((c) == 0x0Au || (c) == 0x0Du || (c) == 0x20u || (c) == 'r')
#define TCRV32_IS_UP(c)    ((c) == 0x01u || (c) == 'w' || (c) == 'W')
#define TCRV32_IS_DOWN(c)  ((c) == 0x03u || (c) == 's' || (c) == 'S')
#define TCRV32_IS_LEFT(c)  ((c) == 0x04u || (c) == 'a' || (c) == 'A')
#define TCRV32_IS_RIGHT(c) ((c) == 0x02u || (c) == 'd' || (c) == 'D')

/* 事件层：没有事件返回 0；有事件返回 1 并写 *code / *down（指针可为 NULL） */
int kbd_waiting(void);                     /* 只读 STAT */
int kbd_get_event(int *code, int *down);   /* 先看 STAT 再读 EVENT（读即消费） */
/* 批量抽干（每帧调一次）：把最多 max 条事件写进 codes[]（键码，弹起用 code|0x100 表示），
 * 返回实际条数。不认识的键也会返回，由调用方过滤。 */
int kbd_drain(int *codes, int max);

/* ==================== GPU：内存→内存二维引擎（0x40000600，仅 1 GiB 配置） ====================
 * 一条命令 = 两维循环：行 0..ROWS-1、列 0..LEN-1，dst[行*STRIDE+列] = f(src, ARG0)，
 * 每拍推进 8 字节；按字节写、无需对齐；LEN 不是 8 的倍数时末组自动裁剪。
 * 寄存器：+0x00 VER，+0x04 CMD（写触发，低 8 位为 op），+0x08 DST，+0x0C SRC，
 * +0x10 LEN，+0x14 ROWS，+0x18 STRIDE，+0x1C ARG0，+0x20 ARG1，
 * +0x24 STAT（[0] busy、[1] done、[2] err），+0x28 PIX，+0x2C ACC。
 * 只有一套参数寄存器：STAT.busy 落 0 前不要改写参数。
 */
#define TCRV32_GPU_BASE     0x40000600u
#define TCRV32_GPU_VER      (TCRV32_GPU_BASE + 0x00u)
#define TCRV32_GPU_CMD      (TCRV32_GPU_BASE + 0x04u)
#define TCRV32_GPU_DST      (TCRV32_GPU_BASE + 0x08u)
#define TCRV32_GPU_SRC      (TCRV32_GPU_BASE + 0x0Cu)
#define TCRV32_GPU_LEN      (TCRV32_GPU_BASE + 0x10u)
#define TCRV32_GPU_ROWS     (TCRV32_GPU_BASE + 0x14u)
#define TCRV32_GPU_STRIDE   (TCRV32_GPU_BASE + 0x18u)
#define TCRV32_GPU_ARG0     (TCRV32_GPU_BASE + 0x1Cu)
#define TCRV32_GPU_ARG1     (TCRV32_GPU_BASE + 0x20u)
#define TCRV32_GPU_STAT     (TCRV32_GPU_BASE + 0x24u)
#define TCRV32_GPU_PIX      (TCRV32_GPU_BASE + 0x28u)
#define TCRV32_GPU_ACC      (TCRV32_GPU_BASE + 0x2Cu)

#define TCRV32_GPU_OP_CONST 0x01u   /* dst[i] = ARG0（清屏/填色） */
#define TCRV32_GPU_OP_COPY  0x02u   /* dst[i] = src[i] */
#define TCRV32_GPU_OP_ADD   0x03u
#define TCRV32_GPU_OP_SUB   0x04u
#define TCRV32_GPU_OP_MIN   0x05u
#define TCRV32_GPU_OP_MAX   0x06u
#define TCRV32_GPU_OP_AVG   0x07u   /* (src[i] + dst[i]) >> 1 */
#define TCRV32_GPU_OP_AND   0x08u
#define TCRV32_GPU_OP_OR    0x09u
#define TCRV32_GPU_OP_XOR   0x0Au
#define TCRV32_GPU_OP_SUM   0x0Bu   /* ACC += src[i]（不写内存） */

#define TCRV32_GPU_BPT      8u      /* 每拍字节数（硬件指标；软件不依赖它） */

void     gpu_wait(void);                 /* 等 busy 落 0（参数寄存器唯一，必须等） */
uint32_t gpu_ver(void);
uint32_t gpu_stat(void);
uint32_t gpu_pix(void);
/* 二维运算：dst 起点 + LEN/ROWS/STRIDE + op + ARG0 */
void     gpu_run2d(uint32_t op, uint32_t dst, uint32_t src,
                   uint32_t len, uint32_t rows, uint32_t stride, uint32_t arg0);
/* 常用两件：填色块（= CONST）与搬块（= COPY） */
void     gpu_fill_rect(uint32_t dst, uint32_t len, uint32_t rows, uint32_t stride, uint32_t color);
void     gpu_copy_rect(uint32_t dst, uint32_t src, uint32_t len, uint32_t rows, uint32_t stride);
/* 扫描线专用：先 gpu_span_begin() 把 ROWS 设成 1，之后每行一条 gpu_fill_span() */
void     gpu_span_begin(void);
void     gpu_fill_span(uint32_t dst, uint32_t span, uint32_t color);

/* ==================== 串口式控制台（ECALL / OUT） ==================== */
void          putch(char c);
void          putstr(const char *s);
long          getch(void);
unsigned long time_ms(void);
void          putdec(unsigned long v);
void          puthex(uint32_t v);
void          putdec_s(long v);
int           printf(const char *fmt, ...);   /* %d %u %x %s %c %% */

void sys_exit(int code) __attribute__((noreturn));

/* ==================== 堆（bump 分配器，support/malloc.c） ==================== */
/* bump 分配器：16 字节对齐；free 不回收；仅最后一块可原地 realloc。 */
void  *malloc(size_t n);
void   free(void *p);
void  *calloc(size_t n, size_t sz);
void  *realloc(void *p, size_t n);
size_t heap_free(void);            /* 剩余堆字节数（调试用） */

/* ==================== 内存 / 字符串 ==================== */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);

#ifdef __cplusplus
}
#endif
#endif /* TCRV32_H */
