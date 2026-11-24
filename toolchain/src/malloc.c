/* malloc.c —— TCRV32 的 bump allocator（够用、可预测、无碎片）
 *
 * 堆区间：[_end, _stack_top - 1 MiB)  ← 给栈留 1 MiB（DMEM 扩到 256 MiB 后放宽）
 * free() 是空操作；realloc 只在"最后一块"原地扩展，否则重新分配 + 拷贝。
 * 没有 brk 系统调用，全部在用户态完成。
 */
#include <stdint.h>
#include <stddef.h>

extern char _end[];          /* link.ld: bss 末尾 */
extern char _stack_top[];    /* link.ld: 栈顶（1 GiB 地图下为 0x11000000） */

#if defined(TCRV32_MAP_16M)
#  define HEAP_RESERVE  8192u              /* 16 MiB 地图：给栈留 8 KiB */
#else
#  define HEAP_RESERVE  (1024u * 1024u)    /* 1 GiB 地图：给栈留 1 MiB */
#endif

static uintptr_t g_brk;      /* 0 = 还没初始化 */

static uintptr_t heap_start(void) {
    if (!g_brk) {
        uintptr_t p = (uintptr_t)_end;
        p = (p + 15u) & ~(uintptr_t)15u;      /* 16 字节对齐 */
        g_brk = p;
    }
    return g_brk;
}

static uintptr_t heap_limit(void) {
    return (uintptr_t)_stack_top - HEAP_RESERVE;
}

void *malloc(size_t n) {
    if (!n) return 0;
    uintptr_t base = heap_start();
    size_t need = (n + 15u) & ~(size_t)15u;   /* 16 字节对齐，便于 double/结构体 */
    if (base + need > heap_limit()) return 0;
    g_brk = base + need;
    return (void *)base;
}

void free(void *p) { (void)p; }               /* bump：不回收 */

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    if (sz && total / sz != n) return 0;      /* 溢出检查 */
    void *p = malloc(total);
    if (p) {
        volatile char *q = (volatile char *)p;
        for (size_t i = 0; i < total; i++) q[i] = 0;
    }
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    /* 只有"最后一块"才能原地扩 */
    if ((uintptr_t)p + ((n + 15u) & ~(size_t)15u) == g_brk &&
        (uintptr_t)p + ((n + 15u) & ~(size_t)15u) <= heap_limit()) {
        g_brk = (uintptr_t)p + ((n + 15u) & ~(size_t)15u);
        return p;
    }
    void *q = malloc(n);
    if (!q) return 0;
    /* 保守拷贝：旧块大小未知，拷贝 n 字节（bump 分配不会越界读到别的活块） */
    volatile char *d = (volatile char *)q, *s = (volatile char *)p;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return q;
}

/* 便于调试：还剩多少堆 */
size_t heap_free(void) {
    uintptr_t base = heap_start();
    uintptr_t lim = heap_limit();
    return (lim > base) ? (size_t)(lim - g_brk) : 0;
}
