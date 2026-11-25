/* gpu.c — GPU（内存→内存二维引擎）驱动
 *
 * 参数寄存器只有一套：提交下一条命令前必须先 gpu_wait()。
 * CMD 是写触发，参数要先写；按字节写，LEN 不要求 8 的倍数；
 * STRIDE 是行间距（字节），一维操作时 ROWS=1。
 */
#include "tcrv32.h"

#define GPU_REG(a) (*(volatile uint32_t *)(a))

/* ROWS 影子寄存器：fill_rect 用行数，fill_span 要求 1，两者交替时必须重设。 */
static uint32_t gpu_rows_now = 0xFFFFFFFFu;

static void gpu_set_rows(uint32_t rows) {
    if (rows == gpu_rows_now) return;
    GPU_REG(TCRV32_GPU_ROWS) = rows;
    gpu_rows_now = rows;
}

void gpu_wait(void) {
    while (GPU_REG(TCRV32_GPU_STAT) & 1u) { }
}

uint32_t gpu_ver(void)  { return GPU_REG(TCRV32_GPU_VER); }
uint32_t gpu_stat(void) { return GPU_REG(TCRV32_GPU_STAT); }
uint32_t gpu_pix(void)  { return GPU_REG(TCRV32_GPU_PIX); }

void gpu_run2d(uint32_t op, uint32_t dst, uint32_t src,
               uint32_t len, uint32_t rows, uint32_t stride, uint32_t arg0) {
    gpu_wait();                                  /* ① 参数寄存器唯一 */
    GPU_REG(TCRV32_GPU_DST)    = dst;
    GPU_REG(TCRV32_GPU_SRC)    = src;
    GPU_REG(TCRV32_GPU_LEN)    = len;
    gpu_set_rows(rows);
    GPU_REG(TCRV32_GPU_STRIDE) = stride;
    GPU_REG(TCRV32_GPU_ARG0)   = arg0;
    GPU_REG(TCRV32_GPU_CMD)    = op & 0xFFu;     /* ② 写触发 */
}

void gpu_fill_rect(uint32_t dst, uint32_t len, uint32_t rows, uint32_t stride, uint32_t color) {
    gpu_run2d(TCRV32_GPU_OP_CONST, dst, dst, len, rows, stride, color & 0xFFu);
}

void gpu_copy_rect(uint32_t dst, uint32_t src, uint32_t len, uint32_t rows, uint32_t stride) {
    gpu_run2d(TCRV32_GPU_OP_COPY, dst, src, len, rows, stride, 0);
}

/* 水平 span（1 像素高）：调用前 ROWS 需已为 1（gpu_span_begin 设一次）。 */
void gpu_span_begin(void) { gpu_wait(); gpu_set_rows(1); }

void gpu_fill_span(uint32_t dst, uint32_t span, uint32_t color) {
    gpu_wait();
    gpu_set_rows(1);                        /* 上一次可能是 fill_rect：这里兜底改写一次 */
    GPU_REG(TCRV32_GPU_DST)    = dst;
    GPU_REG(TCRV32_GPU_LEN)    = span;
    GPU_REG(TCRV32_GPU_STRIDE) = span;      /* 一维：行距 = 行长 */
    GPU_REG(TCRV32_GPU_ARG0)   = color & 0xFFu;
    GPU_REG(TCRV32_GPU_CMD)    = TCRV32_GPU_OP_CONST;
}
