/*
 * libc_mini.c  -  Minimal C library for the seL4 benchmark.
 *
 * Provides: malloc/calloc/free, all string functions, ctype, math,
 * sorting, environment stubs, and process stubs.
 *
 * Heap: a static bump allocator backed by the sel4_heap_base memory
 * region mapped from bench.system (384 MB).  The engine's three calloc
 * calls (small zone 512 KB, main zone 24 MB, hunk 128 MB) dominate.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Forward declarations for things defined in fs_cpio.c or libc_printf.c */
extern void microkit_dbg_puts(const char *);

/* -----------------------------------------------------------------------
 * Heap allocator (bump + free-list)
 * ----------------------------------------------------------------------- */
extern uintptr_t sel4_heap_base;   /* from entry.c / bench.system */

#define HEAP_SIZE  (192UL * 1024 * 1024)  /* 192 MB -- matches bench.system heap region */

static uintptr_t heap_cur  = 0;
static uintptr_t heap_end  = 0;
static int       heap_ready = 0;

static void heap_init(void)
{
    if (heap_ready) return;
    heap_cur  = sel4_heap_base;
    heap_end  = sel4_heap_base + HEAP_SIZE;
    heap_ready = 1;
}

/* Each allocation is prefixed with an 8-byte size header */
#define HDR_SZ 8

void *malloc(size_t size)
{
    if (!heap_ready) heap_init();
    size = (size + 7) & ~7u;  /* 8-byte align */
    size_t total = size + HDR_SZ;
    if (heap_cur + total > heap_end) return NULL;
    uintptr_t p = heap_cur;
    *(size_t *)p = size;
    heap_cur += total;
    return (void *)(p + HDR_SZ);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        /* Zero: the heap region is already zeroed by seL4 untyped retypes,
           but let's be explicit for safety. */
        unsigned char *b = p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t new_size)
{
    if (!ptr) return malloc(new_size);
    size_t old_size = *(size_t *)((uintptr_t)ptr - HDR_SZ);
    if (new_size <= old_size) return ptr;
    void *np = malloc(new_size);
    if (!np) return NULL;
    unsigned char *s = ptr, *d = np;
    for (size_t i = 0; i < old_size; i++) d[i] = s[i];
    /* We don't free the old block (bump allocator) */
    return np;
}

void free(void *ptr) { (void)ptr; /* bump allocator -- no freeing */ }

/* -----------------------------------------------------------------------
 * String functions
 * ----------------------------------------------------------------------- */
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else        { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++));
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *r = dst;
    while (n && (*dst++ = *src++)) n--;
    while (n--) *dst++ = '\0';
    return r;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *r = dst;
    dst += strlen(dst);
    while (n-- && *src) *dst++ = *src++;
    *dst = '\0';
    return r;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && (*a|32) == (*b|32)) { a++; b++; }
    return (unsigned char)(*a|32) - (unsigned char)(*b|32);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && (*a|32) == (*b|32)) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)(*a|32) - (unsigned char)(*b|32);
}

char *strchr(const char *s, int c)
{
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (c == 0) ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (str) *saveptr = str;
    char *s = *saveptr;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *saveptr = s; return NULL; }
    char *tok = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s = '\0'; *saveptr = s + 1; }
    else     { *saveptr = s; }
    return tok;
}

char *strtok(char *str, const char *delim)
{
    static char *saved;
    return strtok_r(str, delim, &saved);
}

char *strerror(int e)
{
    (void)e;
    return "error";
}

/* -----------------------------------------------------------------------
 * ctype
 * ----------------------------------------------------------------------- */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isalnum(int c) { return isdigit(c)||isalpha(c); }
int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
int isupper(int c) { return c>='A'&&c<='Z'; }
int islower(int c) { return c>='a'&&c<='z'; }
int isprint(int c) { return c>=0x20&&c<0x7f; }
int ispunct(int c) { return isprint(c)&&!isalnum(c)&&c!=' '; }
int iscntrl(int c) { return (unsigned)c<0x20||c==0x7f; }
int toupper(int c) { return islower(c)?c-32:c; }
int tolower(int c) { return isupper(c)?c+32:c; }

/* -----------------------------------------------------------------------
 * Number conversion
 * ----------------------------------------------------------------------- */
long strtol(const char *s, char **end, int base)
{
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0') { if (s[1]=='x'||s[1]=='X') { base=16; s+=2; } else base=8; }
        else base = 10;
    } else if (base == 16 && *s=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
    long v = 0;
    while (*s) {
        int d;
        if (isdigit((unsigned char)*s)) d = *s - '0';
        else if (isalpha((unsigned char)*s)) d = tolower((unsigned char)*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

long long strtoll(const char *s, char **end, int base)
{
    return (long long)strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    return (unsigned long long)strtol(s, end, base);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s){ return strtol(s, NULL, 10); }

double strtod(const char *s, char **end)
{
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    double v = 0.0;
    while (isdigit((unsigned char)*s)) { v = v*10.0 + (*s++ - '0'); }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (isdigit((unsigned char)*s)) { v += (*s++ - '0') * frac; frac *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (isdigit((unsigned char)*s)) exp = exp*10 + (*s++ - '0');
        double m = 1.0;
        while (exp--) m *= 10.0;
        if (eneg) v /= m; else v *= m;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s)  { return strtod(s, NULL); }

/* -----------------------------------------------------------------------
 * Math functions (software implementations, aarch64 hardware sqrt)
 * ----------------------------------------------------------------------- */
#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962

static double fabs_impl(double x) { return x < 0 ? -x : x; }
double fabs(double x)  { return fabs_impl(x); }
float  fabsf(float x)  { return x < 0 ? -x : x; }

double floor(double x)
{
    if (x >= 0) return (double)(long long)x;
    long long i = (long long)x;
    return (double)(x < i ? i - 1 : i);
}
float floorf(float x) { return (float)floor(x); }

double ceil(double x)
{
    if (x <= 0) return (double)(long long)x;
    long long i = (long long)x;
    return (double)(x > i ? i + 1 : i);
}
float ceilf(float x) { return (float)ceil(x); }

double fmod(double x, double y)
{
    if (y == 0.0) return x;
    double q = x / y;
    double qi = (q >= 0) ? floor(q) : ceil(q);
    return x - qi * y;
}
float fmodf(float x, float y) { return (float)fmod(x, y); }

/* Hardware sqrt on aarch64 */
double sqrt(double x)
{
    if (x < 0) return 0.0;
    double r;
    __asm__ volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}
float sqrtf(float x)
{
    if (x < 0) return 0.0f;
    float r;
    __asm__ volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}

/* Range-reduce angle to [-pi, pi] */
static double reduce_angle(double x)
{
    x = fmod(x, 2.0 * M_PI);
    if (x >  M_PI) x -= 2.0 * M_PI;
    if (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

/* sin via Taylor series (sufficient accuracy for game physics) */
double sin(double x)
{
    x = reduce_angle(x);
    /* Bring to [-pi/2, pi/2] */
    if (x >  M_PI_2) x = M_PI - x;
    if (x < -M_PI_2) x = -M_PI - x;
    double x2 = x * x;
    return x * (1.0 - x2/6.0 * (1.0 - x2/20.0 * (1.0 - x2/42.0 *
           (1.0 - x2/72.0 * (1.0 - x2/110.0)))));
}
float sinf(float x) { return (float)sin(x); }

double cos(double x)
{
    return sin(x + M_PI_2);
}
float cosf(float x) { return (float)cos(x); }

double tan(double x)
{
    double s = sin(x), c = cos(x);
    return (c != 0.0) ? s / c : 1e300;
}
float tanf(float x) { return (float)tan(x); }

/* atan via minimax polynomial */
static double atan_core(double x)
{
    /* |x| <= 1; Horner form of atan approximation */
    double x2 = x * x;
    return x * (1.0 - x2 * (1.0/3.0 - x2 * (1.0/5.0 - x2 * (1.0/7.0 -
           x2 * (1.0/9.0 - x2 * (1.0/11.0 - x2 * (1.0/13.0)))))));
}

double atan(double x)
{
    int neg = (x < 0);
    if (neg) x = -x;
    double r;
    if (x > 1.0) r = M_PI_2 - atan_core(1.0 / x);
    else         r = atan_core(x);
    return neg ? -r : r;
}
float atanf(float x) { return (float)atan(x); }

double atan2(double y, double x)
{
    if (x == 0.0) {
        if (y > 0.0) return  M_PI_2;
        if (y < 0.0) return -M_PI_2;
        return 0.0;
    }
    double r = atan(y / x);
    if (x < 0.0) r += (y >= 0.0) ?  M_PI : -M_PI;
    return r;
}
float atan2f(float y, float x) { return (float)atan2(y, x); }

double asin(double x)
{
    if (x >=  1.0) return  M_PI_2;
    if (x <= -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x*x));
}
float asinf(float x) { return (float)asin(x); }

double acos(double x)
{
    if (x >=  1.0) return 0.0;
    if (x <= -1.0) return M_PI;
    return atan2(sqrt(1.0 - x*x), x);
}
float acosf(float x) { return (float)acos(x); }

/* exp via Taylor series with range reduction */
double exp(double x)
{
    /* e^x = 2^(x/ln2); reduce to small range */
    if (x > 709.0) return 1e308;
    if (x < -709.0) return 0.0;
    /* Simple series: good for |x| < 1 after range reduction */
    int k = (int)(x * 1.4426950408889634);  /* x * log2(e) */
    double r = x - k * 0.6931471805599453;  /* x - k*ln(2) */
    double e = 1.0 + r * (1.0 + r * (0.5 + r * (1.0/6.0 + r * (1.0/24.0 +
               r * (1.0/120.0 + r * (1.0/720.0))))));
    /* Multiply by 2^k */
    if (k >= 0 && k < 63) { for (int i=0;i<k;i++) e*=2.0; }
    else if (k < 0 && k > -63) { for (int i=0;i<-k;i++) e*=0.5; }
    return e;
}
float expf(float x) { return (float)exp(x); }

/* log via atanh series */
double log(double x)
{
    if (x <= 0.0) return -1e308;
    /* Reduce: x = m * 2^e, m in [1,2) */
    int e = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; e++; }
    while (m < 1.0)  { m *= 2.0; e--; }
    /* ln(m) via atanh: ln(m) = 2*atanh((m-1)/(m+1)) */
    double y = (m - 1.0) / (m + 1.0);
    double y2 = y * y;
    double ln_m = 2.0 * y * (1.0 + y2 * (1.0/3.0 + y2 * (1.0/5.0 +
                  y2 * (1.0/7.0 + y2 * (1.0/9.0)))));
    return ln_m + e * 0.6931471805599453;
}
float logf(float x) { return (float)log(x); }

double log10(double x) { return log(x) * 0.4342944819032518; }
float log10f(float x)  { return (float)log10(x); }

double pow(double x, double y)
{
    if (x == 0.0) return 0.0;
    if (y == 0.0) return 1.0;
    return exp(y * log(fabs_impl(x)));
}
float powf(float x, float y) { return (float)pow(x, y); }

/* -----------------------------------------------------------------------
 * Sorting / searching
 * ----------------------------------------------------------------------- */
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *))
{
    /* Simple insertion sort for small n, quicksort otherwise */
    char *b = base;
    char tmp[256];
    if (size > 256) return;  /* shouldn't happen in engine code */

    /* Insertion sort */
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, b + i*size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j-1)*size, tmp) > 0) {
            memcpy(b + j*size, b + (j-1)*size, size);
            j--;
        }
        memcpy(b + j*size, tmp, size);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid*size);
        if (r == 0) return (void *)(b + mid*size);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Random
 * ----------------------------------------------------------------------- */
static unsigned int rand_state = 12345;
int rand(void)
{
    rand_state = rand_state * 1664525u + 1013904223u;
    return (int)((rand_state >> 1) & 0x7fffffff);
}
void srand(unsigned int seed) { rand_state = seed; }
#define RAND_MAX 0x7fffffff

/* -----------------------------------------------------------------------
 * Time
 * ----------------------------------------------------------------------- */
typedef long time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t time(time_t *t) {
    extern int Sys_Milliseconds(void);
    time_t v = (time_t)(Sys_Milliseconds() / 1000);
    if (t) *t = v;
    return v;
}

double difftime(time_t t1, time_t t0) { return (double)(t1 - t0); }

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

char *ctime(const time_t *t)
{
    /* Stub: l_precomp uses this for __DATE__/__TIME__ macros only.
       Return a constant string to avoid needing a real calendar. */
    (void)t;
    return "Mon Jan  1 00:00:00 2025\n";
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    (void)t;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 125;   /* 2025 */
    return &tm;
}
struct tm *localtime(const time_t *t) { return gmtime(t); }

/* -----------------------------------------------------------------------
 * Environment / process stubs
 * ----------------------------------------------------------------------- */
char *getenv(const char *name) { (void)name; return NULL; }

int setenv(const char *name, const char *val, int ov)
{ (void)name; (void)val; (void)ov; return 0; }

void exit(int code)
{
    extern void microkit_dbg_puts(const char *);
    microkit_dbg_puts("BENCH: exit() called\n");
    (void)code;
    while (1) {}
}

void abort(void)
{
    extern void microkit_dbg_puts(const char *);
    microkit_dbg_puts("BENCH: abort() called\n");
    while (1) {}
}

/* signal() -- no-op on seL4 */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler)
{ (void)sig; (void)handler; return (sighandler_t)0; }

/* -----------------------------------------------------------------------
 * setjmp / longjmp  (AArch64 bare-metal)
 *
 * jmp_buf layout (matches sel4/include/setjmp.h):
 *   [0..9]   x19-x28   callee-saved GP registers
 *   [10..11] x29, x30  frame pointer, link register
 *   [12]     sp         stack pointer
 *   [13..20] d8-d15    callee-saved FP/SIMD registers
 *
 * Uses GCC __attribute__((naked)) so the compiler emits no prologue
 * and our asm IS the complete function body.
 * ----------------------------------------------------------------------- */
typedef long jmp_buf_t[24];

__attribute__((naked)) int setjmp(jmp_buf_t env)
{
    __asm__ (
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x2, sp\n"
        "str x2,       [x0, #96]\n"
        "stp d8,  d9,  [x0, #104]\n"
        "stp d10, d11, [x0, #120]\n"
        "stp d12, d13, [x0, #136]\n"
        "stp d14, d15, [x0, #152]\n"
        "mov x0, #0\n"
        "ret\n"
    );
}

__attribute__((naked, noreturn)) void longjmp(jmp_buf_t env, int val)
{
    __asm__ (
        "ldp x19, x20, [x0, #0]\n"
        "ldp x21, x22, [x0, #16]\n"
        "ldp x23, x24, [x0, #32]\n"
        "ldp x25, x26, [x0, #48]\n"
        "ldp x27, x28, [x0, #64]\n"
        "ldp x29, x30, [x0, #80]\n"
        "ldr x2,       [x0, #96]\n"
        "mov sp, x2\n"
        "ldp d8,  d9,  [x0, #104]\n"
        "ldp d10, d11, [x0, #120]\n"
        "ldp d12, d13, [x0, #136]\n"
        "ldp d14, d15, [x0, #152]\n"
        "mov x0, x1\n"          /* return val */
        "cbnz x0, 1f\n"        /* if val==0, return 1 instead */
        "mov x0, #1\n"
        "1: ret\n"
    );
}

/* POSIX aliases */
__attribute__((naked)) int _setjmp(jmp_buf_t env)
{
    __asm__ (
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x2, sp\n"
        "str x2,       [x0, #96]\n"
        "stp d8,  d9,  [x0, #104]\n"
        "stp d10, d11, [x0, #120]\n"
        "stp d12, d13, [x0, #136]\n"
        "stp d14, d15, [x0, #152]\n"
        "mov x0, #0\n"
        "ret\n"
    );
}

__attribute__((naked, noreturn)) void _longjmp(jmp_buf_t env, int val)
{
    __asm__ (
        "ldp x19, x20, [x0, #0]\n"
        "ldp x21, x22, [x0, #16]\n"
        "ldp x23, x24, [x0, #32]\n"
        "ldp x25, x26, [x0, #48]\n"
        "ldp x27, x28, [x0, #64]\n"
        "ldp x29, x30, [x0, #80]\n"
        "ldr x2,       [x0, #96]\n"
        "mov sp, x2\n"
        "ldp d8,  d9,  [x0, #104]\n"
        "ldp d10, d11, [x0, #120]\n"
        "ldp d12, d13, [x0, #136]\n"
        "ldp d14, d15, [x0, #152]\n"
        "mov x0, x1\n"
        "cbnz x0, 1f\n"
        "mov x0, #1\n"
        "1: ret\n"
    );
}

/* glibc fortified longjmp -- same as longjmp; we don't check shadow stack */
__attribute__((naked, noreturn)) void __longjmp_chk(jmp_buf_t env, int val)
{
    __asm__ (
        "ldp x19, x20, [x0, #0]\n"
        "ldp x21, x22, [x0, #16]\n"
        "ldp x23, x24, [x0, #32]\n"
        "ldp x25, x26, [x0, #48]\n"
        "ldp x27, x28, [x0, #64]\n"
        "ldp x29, x30, [x0, #80]\n"
        "ldr x2,       [x0, #96]\n"
        "mov sp, x2\n"
        "ldp d8,  d9,  [x0, #104]\n"
        "ldp d10, d11, [x0, #120]\n"
        "ldp d12, d13, [x0, #136]\n"
        "ldp d14, d15, [x0, #152]\n"
        "mov x0, x1\n"
        "cbnz x0, 1f\n"
        "mov x0, #1\n"
        "1: ret\n"
    );
}

/* -----------------------------------------------------------------------
 * Additional missing libc symbols
 * ----------------------------------------------------------------------- */

/* strpbrk: find first char in s that's in accept */
char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        const char *a;
        for (a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
    }
    return NULL;
}

/* asctime: format a struct tm as a string */
char *asctime(const void *tm)
{
    (void)tm;
    return "Mon Jan  1 00:00:00 2025\n";
}

/* setvbuf: no-op -- our FILE has no buffering */
int setvbuf(void *stream, char *buf, int mode, size_t size)
{ (void)stream; (void)buf; (void)mode; (void)size; return 0; }

/* clock: return elapsed CPU time in ticks */
typedef long clock_t;
#define CLOCKS_PER_SEC 1000   /* our clock() returns milliseconds */
clock_t clock(void)
{
    extern int Sys_Milliseconds(void);
    return (clock_t)Sys_Milliseconds();
}

/* lrintf: round float to nearest integer */
long lrintf(float x)
{
    if (x >= 0.0f) return (long)(x + 0.5f);
    return (long)(x - 0.5f);
}

/* round: round double to nearest integer, ties away from zero */
double round(double x)
{
    if (x >= 0.0) return (double)(long)(x + 0.5);
    return (double)(long)(x - 0.5);
}

/* -----------------------------------------------------------------------
 * I/O stubs used by libc_printf
 * ----------------------------------------------------------------------- */
int putchar(int c)
{
    char buf[2] = { (char)c, '\0' };
    microkit_dbg_puts(buf);
    return c;
}

int puts(const char *s)
{
    microkit_dbg_puts(s);
    microkit_dbg_puts("\n");
    return 0;
}
