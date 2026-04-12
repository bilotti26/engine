/*
 * libc_printf.c  -  printf / sprintf / sscanf for the seL4 benchmark.
 *
 * Implements:
 *   vsnprintf, snprintf, sprintf
 *   vprintf, printf, fprintf, vfprintf
 *   sscanf (minimal: %d %i %u %lf %f %s %c %% and width modifiers)
 *
 * Float formatting uses the Grisu2-inspired "just good enough" approach:
 * convert via integer arithmetic to avoid needing libm for printf.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* From libc_mini.c */
extern int    isdigit(int);
extern int    isspace(int);
extern size_t strlen(const char *);
extern void  *memcpy(void *, const void *, size_t);
extern void  *memset(void *, int, size_t);
extern double fabs(double);
extern double floor(double);
extern double pow(double, double);
extern double log10(double);

extern void microkit_dbg_puts(const char *);

/* -----------------------------------------------------------------------
 * Low-level output helpers
 * ----------------------------------------------------------------------- */
typedef struct {
    char  *buf;
    size_t pos;
    size_t max;   /* 0 = unbounded (write to serial) */
} outbuf_t;

static void out_char(outbuf_t *o, char c)
{
    if (o->buf) {
        if (o->pos + 1 < o->max)
            o->buf[o->pos++] = c;
    } else {
        char tmp[2] = { c, '\0' };
        microkit_dbg_puts(tmp);
        o->pos++;
    }
}

static void out_str(outbuf_t *o, const char *s, int len)
{
    for (int i = 0; i < len; i++) out_char(o, s[i]);
}

static void out_pad(outbuf_t *o, char c, int n)
{
    while (n-- > 0) out_char(o, c);
}

/* -----------------------------------------------------------------------
 * Integer to string (unsigned)
 * ----------------------------------------------------------------------- */
static int utoa(unsigned long long v, char *buf, int base, int upper)
{
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_hi[] = "0123456789ABCDEF";
    const char *d = upper ? digits_hi : digits_lo;
    char tmp[32];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v) { tmp[n++] = d[v % base]; v /= base; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

/* -----------------------------------------------------------------------
 * Double to string (handles %f, %e, %g)
 * ----------------------------------------------------------------------- */
static int dtoa_f(double v, int prec, char *buf)
{
    /* Handle special cases */
    if (v < 0) { buf[0] = '-'; return 1 + dtoa_f(-v, prec, buf + 1); }
    if (prec < 0) prec = 6;
    if (prec > 15) prec = 15;

    /* Round */
    double rounder = 0.5;
    for (int i = 0; i < prec; i++) rounder *= 0.1;
    v += rounder;

    long long ipart = (long long)v;
    double fpart = v - (double)ipart;

    /* Integer part */
    char ibuf[32];
    int ilen = 0;
    if (ipart == 0) { ibuf[ilen++] = '0'; }
    else {
        long long tmp = ipart;
        char rev[32]; int rn = 0;
        while (tmp) { rev[rn++] = '0' + (int)(tmp % 10); tmp /= 10; }
        for (int i = 0; i < rn; i++) ibuf[ilen++] = rev[rn - 1 - i];
    }
    memcpy(buf, ibuf, ilen);
    int n = ilen;

    if (prec > 0) {
        buf[n++] = '.';
        for (int i = 0; i < prec; i++) {
            fpart *= 10.0;
            int d = (int)fpart;
            if (d > 9) d = 9;
            buf[n++] = '0' + d;
            fpart -= d;
        }
    }
    return n;
}

/* -----------------------------------------------------------------------
 * Core vsnprintf
 * ----------------------------------------------------------------------- */
static int do_vsnprintf(outbuf_t *out, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') { out_char(out, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { out_char(out, '%'); fmt++; continue; }

        /* Flags */
        int flag_left = 0, flag_zero = 0, flag_plus = 0, flag_space = 0;
        for (;;) {
            if (*fmt == '-')      { flag_left  = 1; fmt++; }
            else if (*fmt == '0') { flag_zero  = 1; fmt++; }
            else if (*fmt == '+') { flag_plus  = 1; fmt++; }
            else if (*fmt == ' ') { flag_space = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width*10 + (*fmt++ - '0');

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec*10 + (*fmt++ - '0');
        }

        /* Length modifier */
        int mod_long = 0, mod_longlong = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { mod_longlong = 1; fmt++; }
            else mod_long = 1;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        } else if (*fmt == 'z') {
            mod_long = 1; fmt++;
        }

        char spec = *fmt++;
        char tmp[64];
        int  tlen = 0;
        const char *tptr = NULL;
        char sign = 0;

        switch (spec) {
        case 'd': case 'i': {
            long long v;
            if (mod_longlong)    v = va_arg(ap, long long);
            else if (mod_long)   v = va_arg(ap, long);
            else                 v = va_arg(ap, int);
            if (v < 0) { sign = '-'; v = -v; }
            else if (flag_plus)  sign = '+';
            else if (flag_space) sign = ' ';
            tlen = utoa((unsigned long long)v, tmp, 10, 0);
            tptr = tmp;
            break;
        }
        case 'u': {
            unsigned long long v;
            if (mod_longlong)  v = va_arg(ap, unsigned long long);
            else if (mod_long) v = va_arg(ap, unsigned long);
            else               v = (unsigned)va_arg(ap, unsigned int);
            tlen = utoa(v, tmp, 10, 0);
            tptr = tmp;
            break;
        }
        case 'x': case 'X': {
            unsigned long long v;
            if (mod_longlong)  v = va_arg(ap, unsigned long long);
            else if (mod_long) v = va_arg(ap, unsigned long);
            else               v = (unsigned)va_arg(ap, unsigned int);
            tlen = utoa(v, tmp, 16, spec=='X');
            tptr = tmp;
            break;
        }
        case 'o': {
            unsigned long long v;
            if (mod_longlong)  v = va_arg(ap, unsigned long long);
            else if (mod_long) v = va_arg(ap, unsigned long);
            else               v = (unsigned)va_arg(ap, unsigned int);
            tlen = utoa(v, tmp, 8, 0);
            tptr = tmp;
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            tmp[0] = '0'; tmp[1] = 'x';
            tlen = 2 + utoa(v, tmp + 2, 16, 0);
            tptr = tmp;
            break;
        }
        case 'f': case 'F': {
            double v = va_arg(ap, double);
            if (v < 0) { sign = '-'; v = -v; }
            else if (flag_plus) sign = '+';
            tlen = dtoa_f(v, prec, tmp);
            tptr = tmp;
            break;
        }
        case 'e': case 'E': {
            double v = va_arg(ap, double);
            if (v < 0) { sign = '-'; v = -v; }
            else if (flag_plus) sign = '+';
            int exp = 0;
            if (v != 0.0) {
                exp = (int)floor(log10(v));
                v /= pow(10.0, (double)exp);
            }
            int prc = (prec < 0) ? 6 : prec;
            tlen = dtoa_f(v, prc, tmp);
            tmp[tlen++] = (spec=='e') ? 'e' : 'E';
            tmp[tlen++] = (exp < 0) ? '-' : '+';
            if (exp < 0) exp = -exp;
            if (exp < 10) tmp[tlen++] = '0';
            char ebuf[8]; int en = utoa(exp, ebuf, 10, 0);
            memcpy(tmp + tlen, ebuf, en); tlen += en;
            tptr = tmp;
            break;
        }
        case 'g': case 'G': {
            double v = va_arg(ap, double);
            /* Simple: always use %f */
            if (v < 0) { sign = '-'; v = -v; }
            tlen = dtoa_f(v, (prec < 0 ? 6 : prec), tmp);
            tptr = tmp;
            break;
        }
        case 'c': {
            tmp[0] = (char)va_arg(ap, int);
            tlen = 1; tptr = tmp;
            break;
        }
        case 's': {
            tptr = va_arg(ap, const char *);
            if (!tptr) tptr = "(null)";
            tlen = (int)strlen(tptr);
            if (prec >= 0 && tlen > prec) tlen = prec;
            break;
        }
        case 'n': {
            int *p = va_arg(ap, int *);
            if (p) *p = (int)out->pos;
            continue;
        }
        default:
            out_char(out, '%');
            out_char(out, spec);
            continue;
        }

        /* Compute padding */
        int content_len = (sign ? 1 : 0) + tlen;
        int pad = width - content_len;
        if (pad < 0) pad = 0;

        char pad_char = (flag_zero && !flag_left) ? '0' : ' ';

        if (!flag_left) {
            if (pad_char == '0' && sign) out_char(out, sign);
            out_pad(out, pad_char, pad);
            if (pad_char != '0' && sign) out_char(out, sign);
        } else {
            if (sign) out_char(out, sign);
        }
        out_str(out, tptr, tlen);
        if (flag_left) out_pad(out, ' ', pad);
    }

    /* Null terminate for buffer mode */
    if (out->buf && out->max > 0) {
        if (out->pos < out->max) out->buf[out->pos] = '\0';
        else out->buf[out->max - 1] = '\0';
    }
    return (int)out->pos;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    outbuf_t o = { buf, 0, size };
    return do_vsnprintf(&o, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    outbuf_t o = { NULL, 0, 0 };
    return do_vsnprintf(&o, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* FILE is an opaque type -- we don't need real FILE* here, just enough for
   the engine to call fprintf(stderr, ...) */
typedef void * FILE;
int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    (void)fp;
    return vprintf(fmt, ap);
}
int fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

/* -----------------------------------------------------------------------
 * sscanf: minimal implementation for the engine's three uses:
 *   sscanf(cmd, "cs %i", &csnum)
 *   sscanf(buf, "%lf %lf %lf", &v1, &v2, &v3)
 *   sscanf(str, "%d", &n)  and similar
 * ----------------------------------------------------------------------- */
static double scan_double(const char **sp)
{
    const char *s = *sp;
    while (isspace((unsigned char)*s)) s++;
    int neg = (*s == '-') ? (s++, 1) : (*s == '+') ? (s++, 0) : 0;
    double v = 0;
    while (isdigit((unsigned char)*s)) v = v*10.0 + (*s++ - '0');
    if (*s == '.') {
        s++; double f = 0.1;
        while (isdigit((unsigned char)*s)) { v += (*s++ - '0') * f; f *= 0.1; }
    }
    *sp = s;
    return neg ? -v : v;
}

static long long scan_int(const char **sp, int base)
{
    const char *s = *sp;
    while (isspace((unsigned char)*s)) s++;
    int neg = (*s == '-') ? (s++, 1) : (*s == '+') ? (s++, 0) : 0;
    if (base == 0) {
        if (*s == '0') { if (s[1]=='x'||s[1]=='X') { base=16; s+=2; } else base=8; }
        else base = 10;
    }
    long long v = 0;
    int any = 0;
    while (*s) {
        int d;
        if (isdigit((unsigned char)*s)) d = *s - '0';
        else if (*s>='a'&&*s<='f') d = *s - 'a' + 10;
        else if (*s>='A'&&*s<='F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v*base + d; s++; any = 1;
    }
    *sp = s;
    return (any && neg) ? -v : v;
}

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str;
    int count = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == '%') {
                if (*s == '%') { s++; fmt++; }
                else return count;
                continue;
            }
            /* Optional width */
            int width = 0;
            while (isdigit((unsigned char)*fmt)) width = width*10 + (*fmt++ - '0');
            (void)width; /* TODO: honour width */

            /* Length modifier */
            int mod_long = 0, mod_longlong = 0;
            if (*fmt == 'l') {
                fmt++;
                if (*fmt == 'l') { mod_longlong = 1; fmt++; }
                else mod_long = 1;
            } else if (*fmt == 'h') {
                fmt++; if (*fmt == 'h') fmt++;
            }

            char spec = *fmt++;
            while (isspace((unsigned char)*s)) s++;

            switch (spec) {
            case 'd': {
                long long v = scan_int(&s, 10);
                if (mod_longlong) *va_arg(ap, long long *) = v;
                else if (mod_long) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                count++;
                break;
            }
            case 'i': {
                long long v = scan_int(&s, 0);
                if (mod_longlong) *va_arg(ap, long long *) = v;
                else if (mod_long) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                count++;
                break;
            }
            case 'u': {
                unsigned long long v = (unsigned long long)scan_int(&s, 10);
                if (mod_longlong) *va_arg(ap, unsigned long long *) = v;
                else if (mod_long) *va_arg(ap, unsigned long *) = (unsigned long)v;
                else *va_arg(ap, unsigned *) = (unsigned)v;
                count++;
                break;
            }
            case 'x': case 'X': {
                long long v = scan_int(&s, 16);
                if (mod_longlong) *va_arg(ap, long long *) = v;
                else if (mod_long) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                count++;
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                double v = scan_double(&s);
                if (mod_long || mod_longlong) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                count++;
                break;
            }
            case 'c': {
                *va_arg(ap, char *) = *s++;
                count++;
                break;
            }
            case 's': {
                char *out = va_arg(ap, char *);
                while (isspace((unsigned char)*s)) s++;
                if (!*s) return count;
                while (*s && !isspace((unsigned char)*s)) *out++ = *s++;
                *out = '\0';
                count++;
                break;
            }
            default: break;
            }
        } else if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++;
        } else {
            if (*s == *fmt) { s++; fmt++; }
            else return count;
        }
    }
    return count;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* glibc C99 rename of sscanf and vfprintf -- aliased to our implementations */
int __isoc99_sscanf(const char *str, const char *fmt, ...) __attribute__((alias("sscanf")));
int __vfprintf_chk(FILE *fp, int flag, const char *fmt, va_list ap)
{ (void)flag; return vfprintf(fp, fmt, ap); }
int __fprintf_chk(FILE *fp, int flag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfprintf(fp, fmt, ap); va_end(ap); return r; }
int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap)
{ (void)flag; (void)slen; return vsnprintf(s, n, fmt, ap); }
int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...)
{ (void)flag; (void)slen; va_list ap; va_start(ap, fmt); int r = vsprintf(s, fmt, ap); va_end(ap); return r; }
