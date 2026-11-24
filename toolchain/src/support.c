/* TCRV32 v0.1 最小运行时（freestanding）
 *
 * 两件事：
 *   1. 系统调用封装（ECALL，a7=功能号）与最简输出；
 *   2. 补齐 zig cc / clang 在 freestanding riscv32 下**不提供**的 compiler-rt 辅助函数：
 *      __muldi3 / __divdi3 / __udivdi3 / __moddi3 / __umoddi3 / 移位三件套。
 *      （实测 `python -m ziglang cc --target=riscv32-freestanding-none ... -nostdlib`
 *        会报 __divdi3/__addsf3/__muldf3 未定义 —— 见 ../路线图.md §1.2）
 *
 * 浮点**故意不实现**：平台无 FPU，项目也不用浮点（GPU 走 Q16.16）。
 */
#include <stdint.h>
#include <stddef.h>

/* ---------------- 系统调用 ---------------- */
static inline long sys_call(long n, long a0, long a1, long a2)
{
    register long t0 asm("a0") = a0;
    register long t1 asm("a1") = a1;
    register long t2 asm("a2") = a2;
    register long t3 asm("a7") = n;
    asm volatile("ecall" : "+r"(t0) : "r"(t1), "r"(t2), "r"(t3) : "memory");
    return t0;
}

/* ABI：a7=2 = 输出一个字符（硬件唯一实现的那个）；a7=60 = 退出。
 * putstr 在软件里循环 putch —— 硬件不需要认识字符串指针。 */
void putch(char c)            { sys_call(2, (unsigned char)c, 0, 0); }
void putstr(const char *s)    { while (*s) putch(*s++); }
long getch(void)              { return sys_call(3, 0, 0, 0); }
unsigned long time_ms(void)   { return (unsigned long)sys_call(4, 0, 0, 0); }

void putdec(unsigned long v)
{
    char buf[24];
    int i = 0;
    if (v == 0) { putch('0'); return; }
    while (v && i < 23) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) putch(buf[i]);
}

void puthex(uint32_t v)
{
    static const char *H = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4)
        putch(H[(v >> s) & 0xF]);
}

void putdec_s(long v)
{
    if (v < 0) { putch('-'); putdec((unsigned long)(-v)); }
    else putdec((unsigned long)v);
}

/* 退出（a7=60）。头文件里早就声明了它，但一直没有实现 —— 谁用谁链接失败。
 * 硬件侧 `SCALL` 的 `a7==60` 会拉 halt，所以这里只需要把调用号发出去；
 * 后面的死循环是给"万一硬件没停"兜底（且让编译器放心 noreturn）。 */
void sys_exit(int code)
{
    sys_call(60, code, 0, 0);
    for (;;) { }
}

/* ---------------- 内存函数 ---------------- */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

size_t strlen_(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------------- 64 位辅助（compiler-rt 缺口） ---------------- */
/* 说明：乘法用 32x32→64 的四段拆解，除法用 64 步移位减，都只依赖 RV32IM。 */

uint64_t __muldi3(uint64_t a, uint64_t b)
{
    uint64_t r = 0;
    while (b) {
        if (b & 1) r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (d == 0) { if (rem) *rem = n; return ~(uint64_t)0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= ((uint64_t)1 << i); }
    }
    if (rem) *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)            { return udivmod64(a, b, 0); }
uint64_t __umoddi3(uint64_t a, uint64_t b)
{ uint64_t r; udivmod64(a, b, &r); return r; }

int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t q = udivmod64(ua, ub, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t r;
    udivmod64(ua, ub, &r);
    return (a < 0) ? -(int64_t)r : (int64_t)r;
}

uint64_t __ashldi3(uint64_t a, int s) { return s >= 64 ? 0 : (a << s); }
uint64_t __lshrdi3(uint64_t a, int s) { return s >= 64 ? 0 : (a >> s); }
int64_t  __ashrdi3(int64_t  a, int s)
{ return s >= 64 ? (a < 0 ? -1 : 0) : (a >> s); }
