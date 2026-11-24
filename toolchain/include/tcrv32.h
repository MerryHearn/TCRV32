/* tcrv32.h —— TCRV32（TuringCompleteRISC-V32）平台的统一头文件
 *
 * 用法：把 tools/include 加到 -I 上，然后
 *     #include "tcrv32.h"
 *
 * 覆盖：
 *   · 内存地图常量（RAM / VRAM / MMIO）
 *   · 屏幕：初始化、运行时切模式/偏移、按行列直写
 *   · 文本控制台（屏幕字符矩阵，光标在软件里，自动处理 '\n'）
 *   · 串口式控制台（ECALL/OUT，模拟器里可见）
 *   · 内存 / 字符串 / 简易 printf（**没有 %f**）
 *   · **堆**：malloc / free / calloc / realloc / heap_free（bump 分配器）
 *   · **键盘**：事件队列（`PEEK` / `STAT` / `POP`）+ 键码常量（字符模式）
 *
 * 浮点：**可用**。`+ − × ÷`、比较、int32/int64 ↔ double 由 support/softfloat.c
 *       提供；`float` 走 double 包装（二次舍入无害，逐位正确）。
 *       但没有 libm —— `sqrt`/`sin`/`pow` 之类要自己写。
 *
 * 不提供：libc 主体与标准库头（`stdio.h` 等）、原子（无 A 扩展）、异常、线程。
 * 详见 ../../works/操作规范/11-C与C++支持范围.md
 */
#ifndef TCRV32_H
#define TCRV32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 内存地图 ============================ */
/* ============ 内存地图配置（两套并存，编译期二选一） ============
 * 由构建脚本的 --map 决定，会传 -DTCRV32_MAP_16M 或 -DTCRV32_MAP_1G。
 * 不传时按 1 GiB 处理（默认配置）。
 *   16m   IMEM 1 MiB / DMEM 2 MiB / 文本 1 MiB / 像素 4 MiB / 资源 4 MiB，栈顶 0x00400000
 *   1g    IMEM 16 MiB / DMEM 256 MiB / 文本 16 MiB / 像素 64 MiB / 资源 64 MiB / 空闲 608 MiB
 */
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
/* 屏幕设置窗口（支持多屏）：0x40000200 + 0x10*屏幕号 + 0x04*域
 *   域 0 = 选项号寄存器，域 1 = 值（写值那一拍触发屏幕的"写入设置"）
 *   屏幕 0：0x40000200 / 0x40000204   ← 与旧地址完全一致
 *   屏幕 1：0x40000210 / 0x40000214                                        */
#define TCRV32_SCR_OPT     0x40000200u  /* 屏幕 0 选项号（兼容旧名） */
#define TCRV32_SCR_VAL     0x40000204u  /* 屏幕 0 值（兼容旧名） */
#define TCRV32_SCR_WIN     0x40000200u  /* 窗口基址 */
#define TCRV32_SCR_N       2            /* 支持的屏幕数 */
#define TCRV32_SCR_OPT_OF(id) (TCRV32_SCR_WIN + 0x10u * (uint32_t)(id))
#define TCRV32_SCR_VAL_OF(id) (TCRV32_SCR_WIN + 0x10u * (uint32_t)(id) + 4u)

/* 帧缓冲布局（每块 64 KiB，够 256×192 Pixel8 = 49152 字节；双缓冲各占一块）
 *   FB0       屏 0 主缓冲
 *   FB0_BACK  屏 0 第二块缓冲（双缓冲翻页用）
 *   FB1       屏 1（文本副屏）
 * 注意：Pixel8 下 256×192 需要 49152 字节 < 64 KiB ✓ */

/* ============================ 屏幕 ============================ */
#define SCR_ASCII8       0            /* 每字节 1 字符，96×40 */
#define SCR_ASCII32      1            /* 4 字节：字符/前景/背景 */
#define SCR_PIXEL8       2            /* 每字节 1 像素 RRRGGGBB */
#define SCR_PIXEL32      3            /* 4 字节：R/G/B */

#define SCR_COLS         96
#define SCR_ROWS         40
#define SCR_CELLS        (SCR_COLS * SCR_ROWS)

/* 屏幕的"选项号"（**官方文档**：只有 0/1/2 三个）
 *   0 切换模式      值 = SCR_ASCII8 / SCR_ASCII32 / SCR_PIXEL8 / SCR_PIXEL32
 *   1 设置数据偏移量 值 = 字节偏移
 *   2 选择分辨率    值 = setting；像素模式下
 *                    W = 4×(setting+1)、H = 3×(setting+1)，范围 4×3 .. 1024×768
 * 说明：ASCII 模式的像素尺寸不受 2 号选项影响（96×40）。 */
#define SCR_OPT_MODE     0
#define SCR_OPT_OFFSET   1
#define SCR_OPT_RES      2

/* 像素模式分辨率：setting → 宽/高 */
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

/* 通用：先写选项号寄存器，再写值（值那一拍触发"写入设置"）
 * scr_set()      → 屏幕 0（兼容旧代码）
 * scr_set2(id,…) → 指定屏幕 */
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

/* ============ 图形（M3 能力边界已实测，参数如下） ============
 *
 * 几何：
 *   字符模式  固定 96×40 个字符格，与分辨率选项无关；每格占 1 字节（ASCII8）
 *             或 4 字节（ASCII32：字符、前景、背景、第 4 字节不用）
 *   像素模式  宽高比 4:3，W = 4×(setting+1)，H = 3×(setting+1)，最小 4×3、最大 1024×768
 *             stride 恒等于 W×bpp（没有行填充），所以 gfx_stride = W × bpp
 *   颜色      Pixel8 与 ASCII32 的每个颜色字节都是 RRRGGGBB；Pixel32 每像素 4 字节，R/G/B 各一字节
 *
 * 屏幕选项按模式分别定义，不是全局一套：
 *   0 切换模式            四种模式通用
 *   1 设置数据偏移量      四种模式通用
 *   2 字符模式 = 前景色   像素模式 = 分辨率 setting
 *   3 字符模式 = 背景色
 *   4 ASCII8 = 字体       四档对应 cp437 / cp850 / cp910 / cp866
 *
 * 实测结论（真机验收，见 works/官方资料/04-游戏侧机制.md §9.0）：
 *   · ASCII32 每格自带前景/背景，彩色字符可用
 *   · Pixel8 的 RRRGGGBB 位序正确：0xE0 红、0x1C 绿、0x03 蓝
 *   · Pixel32 的字节顺序是 R、G、B
 *   · 翻页只需改 1 号选项，无需拷贝像素
 *   · 字符模式的 2 号与 3 号选项在本项目上**未观察到效果**，要彩色字符请改用 ASCII32
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
/* 内联：每像素只做 1 次乘法 + 1 次写（原来走函数调用，每像素 30+ 条指令） */
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

/* ==================== 键盘（MMIO 槽 2：0x400–0x5FF） ====================
 *
 * 硬件是"事件队列"，不是"当前按住的键矩阵"：
 *   · 一次按键产生两条事件：按下（down=1）、弹起（down=0）
 *   · 实测**长按不产生重复事件**，所以"按住连续移动"必须由软件自己按时间积分
 *   · 元件在每个时钟周期开始时只能读出队头一个事件 ⇒ 读一次只取走一条，不能"重试"
 *
 * 三个寄存器（硬件语义见 works/总结参考/22-键盘与GPU方案.md §2.3）：
 *   +0x00 PEEK  RO   bit[8]=按下位(1 按下 / 0 弹起)，bit[7:0]=键码，其余位 0；**只看不取**
 *   +0x04 STAT  RO   bit0 = 事件队列非空
 *   +0x08 EVENT **读 = 同一拍取走并弹出**（返回同一个 bit[8:0]）；写 = 只弹出
 *
 * ⚠️ 取事件**必须走 EVENT（同拍读弹）**，不要"先 PEEK 再写 POP"：
 *    若键盘元件的键码/按下位只在"读取下一事件"那一拍有效（游戏自己的 keyboard 指令
 *    就是同拍取样+消费），分两拍读会在真机上恒读到 0。
 *    PEEK 只在硬件确认"引脚常驻有效"时才有意义，留给 `kbdhard.c` 做诊断。
 *
 * ⚠️ **轮询只读 STAT**：读 EVENT 就等于读一次关卡 IN —— 会把队头吃掉、不能"读不到就重试"。
 *    read_next 只高 1 拍，因为 CPU 单周期（1 指令 = 1 tick，MemR 只在那一条 lw 为 1）。
 *
 * 键码是**字符模式**的实测值：字母数字 = ASCII，特殊键是游戏自定的固定码
 * （回车 0x0A 不是 0x0D；Esc 0x0E 不是 0x1B；方向键 0x01–0x04）。
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

/* 退出/返回类按键的约定（2026-11 真机经验）：
 *   · Esc = 0x0E **正常可用**（测键码时就是用 Esc 取的 0x0E）；
 *   · 但遇到过**一次**按 Esc 被游戏 UI 吃掉、直接退回标题（原因未明，疑似与焦点/面板有关）；
 *   · 所以"退出"一律同时认 `q` 与 Esc：Esc 能用就用，被吃掉时 q 兜底。
 *   · `0x1B` 是**另一个键**的码值（来历未确认），不要当成 Esc。
 */
#define TCRV32_IS_QUIT(c)  ((c) == 'q' || (c) == 'Q' || (c) == 0x0Eu)
#define TCRV32_IS_ESC(c)   ((c) == 0x0Eu)

/* 键码在不同机器/配置上不一定完全一致（Shift、回车都出现过两种说法）。
 * 判断功能键**一律用这组宏**，不要写单值比较。 */
#define TCRV32_IS_ENTER(c) ((c) == 0x0Au || (c) == 0x0Du || (c) == 0x20u || (c) == 'r')
#define TCRV32_IS_UP(c)    ((c) == 0x01u || (c) == 'w' || (c) == 'W')
#define TCRV32_IS_DOWN(c)  ((c) == 0x03u || (c) == 's' || (c) == 'S')
#define TCRV32_IS_LEFT(c)  ((c) == 0x04u || (c) == 'a' || (c) == 'A')
#define TCRV32_IS_RIGHT(c) ((c) == 0x02u || (c) == 'd' || (c) == 'D')

/* 事件层：没有事件返回 0；有事件返回 1 并写 *code / *down（指针可为 NULL） */
int kbd_waiting(void);                     /* 只读 STAT */
int kbd_get_event(int *code, int *down);   /* STAT → PEEK → POP，三步不可换序 */
/* 批量抽干（每帧调一次）：把最多 max 条事件写进 codes[]（键码，弹起用 code|0x100 表示），
 * 返回实际条数。不认识的键也会返回，由调用方过滤。 */
int kbd_drain(int *codes, int max);

/* ==================== GPU：内存→内存二维引擎（窗口 0x4000_0600） ====================
 *
 * 语义（设计见 works/总结参考/22-键盘与GPU方案.md §4）：
 *   · 每条命令是一次两维循环：`行 = 0..ROWS-1`、`列 = 0..LEN-1`，
 *     `dst[行*STRIDE + 列] = f(src[...], ARG0)`，**每拍推进 8 个连续字节**；
 *   · 写的是**字节**，所以不需要对齐、不需要读改写；`LEN` 不是 8 的倍数时末组自动裁掉；
 *   · `STAT.busy=0` 表示命令结束、参数寄存器可以改写（**只有一套参数寄存器**）。
 *
 * 寄存器（基址 `0x4000_0600`，偏移同硬件 `addr[5:0]`）：
 *   `+0x00` VER（只读，`0x0002_0000`）
 *   `+0x04` CMD（**写触发**，低 8 位是 op）
 *   `+0x08` DST / `+0x0C` SRC / `+0x10` LEN / `+0x14` ROWS / `+0x18` STRIDE
 *   `+0x1C` ARG0 / `+0x20` ARG1
 *   `+0x24` STAT（`[0]` busy、`[1]` done、`[2]` err）
 *   `+0x28` PIX（本条命令处理的元素数）/ `+0x2C` ACC（`SUM` 的结果）
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
/* 扫描线专用：先 `gpu_span_begin()` 把 ROWS 设成 1，之后每行一条 `gpu_fill_span()` */
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
/* 区间 [_end, _stack_top - 8 KiB)，每次分配 16 字节对齐；free 是空操作。
 * 只在"最后一块"能原地 realloc，否则重新分配 + 拷贝。 */
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
