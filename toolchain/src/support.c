/* support.c — 最小运行时：系统调用封装 + compiler-rt 缺口
 *
 * 1. 系统调用：ecall，a7=2 输出字符，a7=60 退出；
 * 2. freestanding 下编译器不提供的 64 位乘除与移位辅助函数。
 * 浮点不在这里，见 softfloat.c。
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

/* a7=2 输出一个字符，a7=60 退出；putstr 在软件里循环 putch。 */
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

/* 退出：a7=60 触发硬件停机；死循环只是给 noreturn 兜底。 */
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
/* 乘法：32×32→64 四段拆解；除法：64 步移位减。只依赖 RV32IM。 */

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
