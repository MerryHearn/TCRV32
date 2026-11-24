/* softfloat.c —— TCRV32 的软件浮点（double 核心 + float 包装 + 整数转换）
 *
 * 全部用整数位运算实现；函数体里**没有任何浮点运算**（否则自己调自己）。
 * 唯一例外：float 的 +−×÷ 直接借 double 做，依据是"二次舍入无害"定理
 *   （p_宽 ≥ 2·p_窄 + 2；53 ≥ 50 ✓），对 binary32 的 + − × ÷ 与原生逐位相同。
 * 舍入模式：就近-偶数（RNE），与 RISC-V 默认一致。
 *
 * 统一约定（贯穿全文件）：
 *   · 拆包后：value = m * 2^(e-52)，m 在 bit52 有隐含位（次正规则 m 更小）
 *   · round53 入口：value = m * 2^(e-55)，m 已归一化到 bit55
 *   · round53 出口：value = m * 2^(e-52)，m 为 53 位
 */

#include <stdint.h>

typedef union { uint64_t u; double d; } DU;
typedef union { uint32_t u; float  f; } FU;

#define FSIGN(u)   ((int)((u) >> 63))
#define FEXP(u)    ((int)(((u) >> 52) & 0x7FFu))
#define FFRAC(u)   ((u) & 0xFFFFFFFFFFFFFULL)
#define QNAN       0x7FF8000000000000ULL
#define SNAN       0x7FF0000000000001ULL
#define F64_MASK   0xFFFFFFFFFFFFFULL
#define BIAS       1023

static double   mk(int sign, int exp, uint64_t frac) {
    DU r;
    r.u = ((uint64_t)(sign & 1) << 63) | ((uint64_t)(exp & 0x7FF) << 52) | (frac & F64_MASK);
    return r.d;
}
static uint64_t bits_of(double d)     { DU r; r.d = d; return r.u; }
static double   from_bits(uint64_t u) { DU r; r.u = u; return r.d; }

static int d_is_nan(double x)  { uint64_t u = bits_of(x); return FEXP(u) == 0x7FF && FFRAC(u) != 0; }
static int d_is_inf(double x)  { uint64_t u = bits_of(x); return FEXP(u) == 0x7FF && FFRAC(u) == 0; }
static int d_is_zero(double x) { return (bits_of(x) & 0x7FFFFFFFFFFFFFFFULL) == 0; }

/* 拆包：value = m * 2^(e-52) */
static void d_unpack(double x, int *sign, uint64_t *m, int *e) {
    uint64_t u = bits_of(x);
    int ex = FEXP(u);
    *sign = FSIGN(u);
    if (ex == 0) { *m = FFRAC(u); *e = 1 - BIAS; }
    else         { *m = FFRAC(u) | (1ULL << 52); *e = ex - BIAS; }
}

/* 归一化到 56 位（bit55 为首 1）：value = m*2^(e-55)
 * 两个方向都要支持：
 *   · 太短（bit55 为 0）→ 左移，e 减
 *   · 太长（bit56 及以上有 1）→ 右移，移出的位并成 sticky（OR 到 bit0），e 加
 * 加法两个 53 位尾数相加会上到 bit56，必须走右移分支。 */
static void norm55(uint64_t *m, int *e) {
    if (!*m) return;
    while (*m >> 56) {
        uint64_t st = *m & 1;
        *m >>= 1;
        (*e)++;
        if (st) *m |= 1;
    }
    while (!(*m & (1ULL << 55))) { *m <<= 1; (*e)--; }
}

/* 入口 value = m*2^(e-55)（m 已归一化）→ 出口 value = m*2^(e-52)，m 为 53 位 */
static void round53(uint64_t *m, int *e, int *sign) {
    if (!*m) { *sign = 0; *e = 0; return; }
    uint64_t guard = (*m >> 2) & 1;
    uint64_t rest  = *m & 0x3;
    *m >>= 3;
    if (guard && (rest || (*m & 1))) {
        *m += 1;
        if (*m == (1ULL << 53)) { *m >>= 1; (*e)++; }
    }
}

/* 入口 value = m*2^(e-52)，m 为 53 位 → 组装成 double */
static double d_pack(int sign, uint64_t m, int e) {
    if (!m) return mk(sign, 0, 0);
    int exp = e + BIAS;
    if (exp >= 0x7FF) return mk(sign, 0x7FF, 0);
    if (exp <= 0) {                              /* 次正规 / 下溢 */
        int sh = 1 - exp;
        if (sh > 63) return mk(sign, 0, 0);
        uint64_t g  = (sh > 0) ? ((m >> (sh - 1)) & 1) : 0;
        uint64_t st = (sh > 1) ? ((m & ((1ULL << (sh - 1)) - 1)) != 0) : 0;
        uint64_t q  = (sh > 0) ? (m >> sh) : m;
        if (g && (st || (q & 1))) q++;
        return mk(sign, 0, q & F64_MASK);
    }
    return mk(sign, exp, m & F64_MASK);
}

/* ---------- 64×64 → 128（32 位分肢） ---------- */
static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t ah = a >> 32, al = a & 0xFFFFFFFFULL;
    uint64_t bh = b >> 32, bl = b & 0xFFFFFFFFULL;
    uint64_t p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    *lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

/* ================= 加 / 减 ================= */
static double add_core(double x, double y, int neg_y) {
    if (d_is_nan(x) || d_is_nan(y)) return from_bits(QNAN);
    if (neg_y) y = from_bits(bits_of(y) ^ 0x8000000000000000ULL);

    if (d_is_inf(x) || d_is_inf(y)) {
        if (d_is_inf(x) && d_is_inf(y) && FSIGN(bits_of(x)) != FSIGN(bits_of(y)))
            return from_bits(QNAN);
        return d_is_inf(x) ? x : y;
    }

    int sa, sb, ea, eb;
    uint64_t ma, mb;
    d_unpack(x, &sa, &ma, &ea);
    d_unpack(y, &sb, &mb, &eb);
    if (!ma && !mb) return mk(0, 0, 0);

    uint64_t a = ma << 3, b = mb << 3;      /* → value = a*2^(ea-55) */
    if (ea < eb) {
        uint64_t t = a; a = b; b = t;
        int ti = ea; ea = eb; eb = ti;
        ti = sa; sa = sb; sb = ti;
    }
    int d = ea - eb;
    if (d) {
        if (d > 63) { if (b) b = 1; else b = 0; }
        else {
            uint64_t sticky = (b & ((1ULL << d) - 1)) != 0;
            b >>= d;
            if (sticky) b |= 1;
        }
    }
    int rs; uint64_t r;
    if (sa == sb) { r = a + b; rs = sa; }
    else if (a >= b) { r = a - b; rs = sa; }
    else { r = b - a; rs = sb; }
    if (!r) return mk(0, 0, 0);
    norm55(&r, &ea);
    round53(&r, &ea, &rs);
    return d_pack(rs, r, ea);
}

double __adddf3(double a, double b) { return add_core(a, b, 0); }
double __subdf3(double a, double b) { return add_core(a, b, 1); }
double __negdf2(double a) { return from_bits(bits_of(a) ^ 0x8000000000000000ULL); }
double __copysigndf(double m, double s) {
    uint64_t um = bits_of(m), us = bits_of(s);
    return from_bits((um & 0x7FFFFFFFFFFFFFFFULL) | (us & 0x8000000000000000ULL));
}

/* ================= 乘法 ================= */
double __muldf3(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return from_bits(QNAN);
    int sx, sy, ex, ey;
    uint64_t mx, my;
    d_unpack(x, &sx, &mx, &ex);
    d_unpack(y, &sy, &my, &ey);
    int rs = sx ^ sy;
    if (d_is_inf(x) || d_is_inf(y)) {
        if (d_is_zero(x) || d_is_zero(y)) return from_bits(QNAN);
        return mk(rs, 0x7FF, 0);
    }
    if (!mx || !my) return mk(rs, 0, 0);

    /* 到 53 位满精度 */
    while (!(mx & (1ULL << 52))) { mx <<= 1; ex--; }
    while (!(my & (1ULL << 52))) { my <<= 1; ey--; }

    uint64_t hi, lo;
    mul64(mx, my, &hi, &lo);                 /* 乘积 < 2^106 */
    uint64_t m = (hi << 14) | (lo >> 50);    /* 取高 56 位 */
    if (lo & ((1ULL << 50) - 1)) m |= 1;     /* sticky */
    int e = ex + ey + 1;                     /* value = m*2^(e-55) */
    int s = rs;
    norm55(&m, &e);
    round53(&m, &e, &s);
    return d_pack(s, m, e);
}

/* ================= 除法 ================= */
double __divdf3(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return from_bits(QNAN);
    int sx, sy, ex, ey;
    uint64_t mx, my;
    d_unpack(x, &sx, &mx, &ex);
    d_unpack(y, &sy, &my, &ey);
    int rs = sx ^ sy;
    if (d_is_inf(x)) {
        if (d_is_inf(y)) return from_bits(QNAN);
        return mk(rs, 0x7FF, 0);
    }
    if (d_is_inf(y)) return mk(rs, 0, 0);
    if (!my) { if (!mx) return from_bits(QNAN); return mk(rs, 0x7FF, 0); }
    if (!mx) return mk(rs, 0, 0);

    while (!(mx & (1ULL << 52))) { mx <<= 1; ex--; }
    while (!(my & (1ULL << 52))) { my <<= 1; ey--; }

    /* 先保证 mx < my：这样商落在 (0.5,1)，首 1 必在 bit55 */
    if (mx >= my) { my <<= 1; ey--; }
    /* 移位相除：q = floor(mx * 2^56 / my)，56 位且 bit55 = 1 */
    uint64_t q = 0, rem = mx;
    for (int i = 0; i < 56; i++) {
        rem <<= 1; q <<= 1;
        if (rem >= my) { rem -= my; q |= 1; }
    }
    if (rem) q |= 1;                       /* sticky */
    int e = ex - ey - 1;                   /* value = q*2^(e-55) */
    int s = rs;
    norm55(&q, &e);
    round53(&q, &e, &s);
    return d_pack(s, q, e);
}

/* ================= 比较 ================= */
int tcrv32_dcmp(double x, double y) {
    uint64_t ux = bits_of(x), uy = bits_of(y);
    if (d_is_zero(x) && d_is_zero(y)) return 0;
    int sx = FSIGN(ux), sy = FSIGN(uy);
    if (sx != sy) return sx ? -1 : 1;
    int mag = (ux < uy) ? -1 : (ux > uy ? 1 : 0);
    return sx ? -mag : mag;
}
int __eqdf2(double x, double y) {
    int nu = d_is_nan(x) || d_is_nan(y);
    return nu ? 1 : (tcrv32_dcmp(x, y) != 0);
}
int __nedf2(double x, double y) {
    if (d_is_nan(x)) return 1;
    if (d_is_nan(y)) return 1;
    return (tcrv32_dcmp(x, y) != 0) ? 1 : 0;
}
int __ltdf2(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    return (tcrv32_dcmp(x, y) < 0) ? -1 : 1;
}
int __ledf2(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    int c = tcrv32_dcmp(x, y);
    return (c <= 0) ? -1 : 1;
}
int __gtdf2(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    int c = tcrv32_dcmp(x, y);
    return (c > 0) ? 1 : -1;
}
int __gedf2(double x, double y) {
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    return (tcrv32_dcmp(x, y) >= 0) ? 1 : -1;
}
int __unorddf2(double x, double y) {
    if (d_is_nan(x)) return 1;
    if (d_is_nan(y)) return 1;
    return 0;
}

/* ================= 整数 ↔ double ================= */
double __floatsidf(int32_t v) {
    if (!v) return mk(0, 0, 0);
    int s = v < 0;
    uint64_t m = s ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
    int e = 55;
    norm55(&m, &e);
    round53(&m, &e, &s);
    return d_pack(s, m, e);
}
double __floatunsidf(uint32_t v) {
    if (!v) return mk(0, 0, 0);
    uint64_t m = v; int s = 0, e = 55;
    norm55(&m, &e);
    round53(&m, &e, &s);
    return d_pack(s, m, e);
}
double __floatdidf(int64_t v) {
    if (!v) return mk(0, 0, 0);
    int s = v < 0;
    uint64_t m = s ? (uint64_t)(-v) : (uint64_t)v;
    int e = 55;
    /* m 可能超过 55 位：先右移到 bit55，sticky 并进去 */
    if (m >> 56) {
        uint64_t st = 0;
        while (m >> 56) { st |= (m & 1); m >>= 1; e++; }
        if (st) m |= 1;
    }
    norm55(&m, &e);
    round53(&m, &e, &s);
    return d_pack(s, m, e);
}
double __floatundidf(uint64_t v) {
    if (!v) return mk(0, 0, 0);
    uint64_t m = v; int s = 0, e = 55;
    if (m >> 56) {
        uint64_t st = 0;
        while (m >> 56) { st |= (m & 1); m >>= 1; e++; }
        if (st) m |= 1;
    }
    norm55(&m, &e);
    round53(&m, &e, &s);
    return d_pack(s, m, e);
}

int32_t __fixdfsi(double x) {
    if (d_is_nan(x)) return 0;
    if (d_is_inf(x)) return FSIGN(bits_of(x)) ? (int32_t)0x80000000 : (int32_t)0x7FFFFFFF;
    int s, e; uint64_t m;
    d_unpack(x, &s, &m, &e);
    if (!m) return 0;
    while (!(m & (1ULL << 52))) { m <<= 1; e--; }
    int sh = e - 52;
    int64_t r;
    if (sh >= 64) r = (int64_t)0x7FFFFFFFFFFFFFFFULL;
    else if (sh >= 0) r = (int64_t)(m << sh);
    else { int d = -sh; r = (d >= 64) ? 0 : (int64_t)(m >> d); }
    if (s) r = -r;
    if (r > 0x7FFFFFFFLL) return (int32_t)0x7FFFFFFF;
    if (r < -0x80000000LL) return (int32_t)0x80000000;
    return (int32_t)r;
}
uint32_t __fixunsdfsi(double x) {
    if (d_is_nan(x) || (FSIGN(bits_of(x)) && !d_is_zero(x))) return 0;
    int32_t v = __fixdfsi(x);
    return (v < 0) ? 0u : (uint32_t)v;
}
int64_t __fixdfdi(double x) {
    if (d_is_nan(x)) return 0;
    int s, e; uint64_t m;
    d_unpack(x, &s, &m, &e);
    if (!m) return 0;
    while (!(m & (1ULL << 52))) { m <<= 1; e--; }
    int sh = e - 52;
    uint64_t r;
    if (sh >= 64) r = 0x7FFFFFFFFFFFFFFFULL;
    else if (sh >= 0) r = (sh >= 64) ? 0 : (m << sh);
    else { int d = -sh; r = (d >= 64) ? 0 : (m >> d); }
    return s ? -(int64_t)r : (int64_t)r;
}
uint64_t __fixunsdfdi(double x) {
    if (d_is_nan(x) || FSIGN(bits_of(x))) return 0;
    int s, e; uint64_t m;
    d_unpack(x, &s, &m, &e);
    if (!m) return 0;
    while (!(m & (1ULL << 52))) { m <<= 1; e--; }
    int sh = e - 52;
    if (sh >= 64) return 0x7FFFFFFFFFFFFFFFULL;
    if (sh >= 0) return m << sh;
    { int d = -sh; return (d >= 64) ? 0 : (m >> d); }
}

/* ================= float ↔ double（必须手搓，不能互相 cast） ================= */
double __extendsfdf2(float a) {
    FU v; v.f = a;
    uint32_t u = v.u;
    int s = (int)(u >> 31), e = (int)((u >> 23) & 0xFF);
    uint32_t f = u & 0x7FFFFFu;
    if (e == 0xFF) return mk(s, 0x7FF, ((uint64_t)f) << 29);     /* inf / NaN */
    if (e == 0) {
        if (!f) return mk(s, 0, 0);
        /* 单精度次正规 → 双精度正规 */
        int sh = 0;
        while (!(f & 0x800000u)) { f <<= 1; sh++; }
        f &= 0x7FFFFFu;
        return mk(s, 1 - 127 + BIAS - sh, ((uint64_t)f) << 29);
    }
    return mk(s, e - 127 + BIAS, ((uint64_t)f) << 29);
}

float __truncdfsf2(double a) {
    int s, e; uint64_t m;
    uint64_t u = bits_of(a);
    if (FEXP(u) == 0x7FF) {                       /* inf / NaN */
        FU r; r.u = ((uint32_t)(u >> 63) << 31) | 0x7F800000u | (uint32_t)(FFRAC(u) >> 29);
        return r.f;
    }
    d_unpack(a, &s, &m, &e);
    if (!m) { FU r; r.u = (uint32_t)s << 31; return r.f; }
    while (!(m & (1ULL << 52))) { m <<= 1; e--; }
    /* value = m * 2^(e-52)；目标 24 位尾数 */
    int exp = e + 127;
    if (exp > 0xFE) { FU r; r.u = ((uint32_t)s << 31) | 0x7F800000u; return r.f; }
    if (exp <= 0) {                               /* 单精度次正规（或 0） */
        int sh = 1 - exp + 29;
        if (sh > 63) { FU r; r.u = (uint32_t)s << 31; return r.f; }
        uint64_t g = (m >> (sh - 1)) & 1;
        uint64_t st = (sh > 1) ? ((m & ((1ULL << (sh - 1)) - 1)) != 0) : 0;
        uint64_t q = m >> sh;
        if (g && (st || (q & 1))) q++;
        FU r; r.u = ((uint32_t)s << 31) | (uint32_t)(q & 0x7FFFFFu);
        return r.f;
    }
    uint64_t g = (m >> 28) & 1, st = (m & ((1ULL << 28) - 1)) != 0;
    uint64_t q = m >> 29;                         /* 24 位（隐含位在 bit23） */
    if (g && (st || (q & 1))) {
        q++;
        if (q == (1ULL << 24)) { q >>= 1; exp++; if (exp > 0xFE) { FU r; r.u = ((uint32_t)s << 31) | 0x7F800000u; return r.f; } }
    }
    FU r;
    r.u = ((uint32_t)s << 31) | ((uint32_t)exp << 23) | (uint32_t)(q & 0x7FFFFFu);
    return r.f;
}

/* ================= float 的算术（借 double，定理保证逐位正确） ================= */
float __addsf3(float a, float b) { return __truncdfsf2(__adddf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __subsf3(float a, float b) { return __truncdfsf2(__subdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __mulsf3(float a, float b) { return __truncdfsf2(__muldf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __divsf3(float a, float b) { return __truncdfsf2(__divdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __negsf2(float a) { FU v; v.f = a; v.u ^= 0x80000000u; return v.f; }

int __eqsf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    return (d_is_nan(x) || d_is_nan(y)) ? 1 : (tcrv32_dcmp(x, y) != 0);
}
int __nesf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    return (tcrv32_dcmp(x, y) != 0) ? 1 : 0;
}
int __ltsf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    int c = tcrv32_dcmp(x, y);
    return (c < 0) ? -1 : 1;
}
int __lesf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    int c = tcrv32_dcmp(x, y);
    return (c <= 0) ? -1 : 1;
}
int __gtsf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    return (tcrv32_dcmp(x, y) > 0) ? 1 : -1;
}
int __gesf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x) || d_is_nan(y)) return 1;
    int c = tcrv32_dcmp(x, y);
    return (c >= 0) ? 1 : -1;
}
int __unordsf2(float a, float b) {
    double x = __extendsfdf2(a), y = __extendsfdf2(b);
    if (d_is_nan(x)) return 1;
    if (d_is_nan(y)) return 1;
    return 0;
}

int32_t __fixsfsi(float a) {
    double d = __extendsfdf2(a);
    return __fixdfsi(d);
}
uint32_t __fixunssfsi(float a) {
    double d = __extendsfdf2(a);
    return __fixunsdfsi(d);
}
int64_t __fixsfdi(float a) {
    double d = __extendsfdf2(a);
    return __fixdfdi(d);
}
uint64_t __fixunssfdi(float a) {
    double d = __extendsfdf2(a);
    return __fixunsdfdi(d);
}
float    __floatsisf(int32_t v)  { return __truncdfsf2(__floatsidf(v)); }
float    __floatunsisf(uint32_t v){ return __truncdfsf2(__floatunsidf(v)); }
