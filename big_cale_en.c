/*
 * Big-number expression evaluator (English comments / English messages)
 *
 * Usage
 *     ./calc               run the built-in formulas once, then ask whether to
 *                          evaluate expressions by hand
 *     ./calc --test        run the built-in test suite (PASS/FAIL)
 *     ./calc --help        show this help
 *
 * Supported operators: + - * / % ^ and parentheses.
 * Numbers may be written as 3.14, .5, 1e3, 2.5E-4.
 * A number in memory looks like this:
 *     d      digit string, d[0] is the most significant digit, no decimal
 *            point stored inside (heap allocated, as long as you like)
 *     len    number of significant digits
 *     scale  how many digits sit after the decimal point; the real value is
 *            "digits read as an integer" / 10^scale
 *     sign   1 positive, -1 negative
 *     e.g. -3.14 is stored as sign=-1, d="314", len=3, scale=2
 *
 * Precedence, lowest to highest:
 *     add/sub < mul/div/mod < unary +/- < power
 * The power operator is right associative: 2^3^2 means 2^(3^2), and -2^2 means
 * -(2^2). Parsing is recursive descent, one function per precedence level, so
 * adding an operator never disturbs the main flow.
 *
 * Semantics worth knowing beforehand
 *   - Division keeps DIV_SCALE decimals, rounds the last digit and strips
 *     trailing zeros: 1/2 prints 0.5
 *   - Modulo follows C's fmod: the sign follows the dividend, -7 % 3 = -1.
 *     The algorithm aligns both decimal points and then takes the integer
 *     remainder, so if both sides were truncated by a division (like 1/3) the
 *     result depends on DIV_SCALE:
 *         (1/3) % (1/3) = 0
 *         1 % (1/3)     = 0.00000000000000000001
 *   - The exponent must be an integer: 2^0.5 is irrational and cannot be
 *     stored in this "finite decimal" representation. The magnitude of the
 *     exponent is unbounded: 2^1000000 works, it just takes a while
 *   - 0^0 = 1; a negative power of 0 is an error
 *   - There is no hard cap on number size; the old SIZE_LIMIT guard is gone.
 *     How far you get depends purely on the machine's memory: it keeps
 *     computing as long as things fit, and only reports "out of memory" when
 *     they really do not (malloc failed, or the digit count grows past what an
 *     int can express) - a physical limit, not a mathematical one
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L     /* keep isatty declared under -std=c99 */
#endif

#if defined(_WIN32)
#include <io.h>
#define STDIN_IS_TTY()  _isatty(0)  /* is standard input a terminal? */
#else
#include <unistd.h>
#define STDIN_IS_TTY()  isatty(0)
#endif

#define DIV_SCALE  20               /* decimals kept by division, last one rounded */

/* ================= small helpers ================= */

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n > 0 ? n : 1);
    if (p == NULL)
        die("out of memory");
    return p;
}

static void *xrealloc(void *old, size_t n)
{
    void *p = realloc(old, n > 0 ? n : 1);
    if (p == NULL)
        die("out of memory");
    return p;
}

/* Advance the cursor past whitespace. The cursor is "a pointer to the scan
   position", so moving the caller's pointer means passing its address */
static void skipBlank(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' ||
           **cursor == '\n' || **cursor == '\r')
        (*cursor)++;
}

/* ================= the big number itself ================= */

typedef struct {
    char *d;      /* digit string, d[0] is the most significant digit */
    int   len;    /* significant digits */
    int   cap;    /* how big d is; realloc when it is not enough */
    int   scale;  /* digits after the decimal point */
    int   sign;   /* 1 or -1 */
} Big;

static void bigSetZero(Big *b)
{
    if (b->d == NULL) {
        b->cap = 2;
        b->d = xmalloc(2);
    }
    b->d[0] = '0';
    b->len = 1;
    b->scale = 0;
    b->sign = 1;
}

static void bigReserve(Big *b, int n)
{
    if (n <= b->cap)
        return;
    int cap = b->cap > 0 ? b->cap : 8;
    while (cap < n) {
        if (cap > INT_MAX / 2) {    /* doubling would overflow, ask for n */
            cap = n;
            break;
        }
        cap *= 2;
    }
    b->d = xrealloc(b->d, (size_t)cap);
    b->cap = cap;
}

static void bigInit(Big *b)
{
    b->d = NULL;
    b->cap = 0;
    bigSetZero(b);
}

static void bigFree(Big *b)
{
    free(b->d);
    b->d = NULL;
    b->cap = 0;
    b->len = 0;
}

/* Build a number out of a small int, used for initialisation */
static void bigSetInt(Big *b, int v)
{
    char buf[16];
    int n = sprintf(buf, "%d", v);
    bigReserve(b, n);
    memcpy(b->d, buf, (size_t)n);
    b->len = n;
    b->scale = 0;
    b->sign = 1;
}

/* Deep copy. Big owns a heap pointer, so a plain struct assignment would leave
   two owners for one buffer */
static void bigAssign(Big *dst, const Big *src)
{
    if (dst == src)
        return;
    bigReserve(dst, src->len);
    memcpy(dst->d, src->d, (size_t)src->len);
    dst->len = src->len;
    dst->scale = src->scale;
    dst->sign = src->sign;
}

static int bigIsZero(const Big *b)
{
    return b->len == 1 && b->d[0] == '0';
}

/* Normalise: drop trailing zeros after the point and leading zeros before it;
   an all-zero value collapses to a plain 0 */
static void bigTrim(Big *b)
{
    while (b->scale > 0 && b->len > 1 && b->d[b->len - 1] == '0') {
        b->len--;
        b->scale--;                  /* a trailing zero after the point does not
                                        change the value */
    }

    int head = 0;
    while (head < b->len - 1 && b->d[head] == '0')
        head++;
    if (head > 0) {
        memmove(b->d, b->d + head, (size_t)(b->len - head));
        b->len -= head;
    }

    if (bigIsZero(b)) {              /* keep 0 as 0, never 0.00 */
        b->scale = 0;
        b->sign = 1;
    }
}

/* Adopt an already-built digit string, saving one copy */
static void bigTake(Big *dst, char *buf, int len, int scale, int sign)
{
    if (len <= 0) {
        free(buf);
        bigSetZero(dst);
        return;
    }
    free(dst->d);
    dst->d = buf;
    dst->cap = len;
    dst->len = len;
    dst->scale = scale;
    dst->sign = sign;
    bigTrim(dst);
}

/* ======== digit-string arithmetic (no sign, no decimal point) ======== */

/* Compare two digit strings: positive if a is larger, negative if b is, 0 if
   they are equal */
static int cmpDigits(const char *a, int alen, const char *b, int blen)
{
    if (alen != blen)          /* more digits wins, given no leading zeros */
        return alen > blen ? 1 : -1;
    int r = memcmp(a, b, (size_t)alen);
    return r > 0 ? 1 : (r < 0 ? -1 : 0);
}

/* Add two digit strings into out (room for max(alen,blen)+1), return the
   resulting length. out must not overlap a or b. Works from the low digit up,
   just like doing it on paper */
static int addAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    int n = alen > blen ? alen : blen;
    int carry = 0;

    for (int i = 0; i < n; i++) {
        int da = (i < alen) ? a[alen - 1 - i] - '0' : 0;   /* take from the ones end */
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        int sum = da + db + carry;
        out[n - 1 - i] = (char)('0' + sum % 10);
        carry = sum / 10;
    }
    if (carry) {                     /* a leftover carry adds one more digit */
        memmove(out + 1, out, (size_t)n);
        out[0] = (char)('0' + carry);
        n++;
    }
    return n;
}

/* Subtract digit strings, caller guarantees a >= b; write into out (room for
   alen), return the length */
static int subAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    int borrow = 0;

    for (int i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0' - borrow;
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        if (da < db) {               /* not enough, borrow from the next digit */
            da += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[alen - 1 - i] = (char)('0' + da - db);
    }

    int head = 0;                    /* borrowing may leave a leading 0 */
    while (head < alen - 1 && out[head] == '0')
        head++;
    if (head > 0) {
        memmove(out, out + head, (size_t)(alen - head));
        alen -= head;
    }
    return alen;
}

/* Multiply a digit string by a single digit (0~9) into out (room for alen+1),
   return the length */
static int mulSmall(const char *a, int alen, int m, char *out)
{
    int carry = 0;

    for (int i = alen - 1; i >= 0; i--) {
        int v = (a[i] - '0') * m + carry;
        out[i] = (char)('0' + v % 10);
        carry = v / 10;
    }
    if (carry) {                     /* the carry pushes out a new digit */
        memmove(out + 1, out, (size_t)alen);
        out[0] = (char)('0' + carry);
        return alen + 1;
    }
    return alen;
}

/* Multiply digit strings: the long multiplication you learned at school.
   out needs room for alen+blen+1 and must not overlap a or b */
static int mulAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    size_t cells = (size_t)(alen + blen + 2);
    long long *acc = xmalloc(sizeof(long long) * cells);
    memset(acc, 0, sizeof(long long) * cells);

    /* Accumulate every digit product into its slot (low end first), no carries yet */
    for (int i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0';
        for (int j = 0; j < blen; j++)
            acc[i + j] += (long long)da * (b[blen - 1 - j] - '0');
    }

    /* Then propagate the carries once */
    long long carry = 0;
    int n = alen + blen;
    for (int k = 0; k < n; k++) {
        long long v = acc[k] + carry;
        acc[k] = v % 10;
        carry = v / 10;
    }
    while (carry) {
        acc[n++] = carry % 10;
        carry /= 10;
    }

    while (n > 1 && acc[n - 1] == 0)   /* leading zeros are pointless */
        n--;
    for (int k = 0; k < n; k++)
        out[k] = (char)('0' + acc[n - 1 - k]);

    free(acc);
    return n;
}

/* Divide digit strings: quotient into quot, remainder into rem. Guess each
   quotient digit, i.e. classic long division.
   quot needs room for alen+1, rem for blen+2 */
static void divmodAbs(const char *a, int alen,
                      const char *b, int blen,
                      char *quot, int *qlen,
                      char *rem,  int *rlen)
{
    char *cur  = xmalloc((size_t)blen + 2);   /* running remainder */
    char *prod = xmalloc((size_t)blen + 2);   /* divisor times one quotient digit */
    int curlen = 0, qn = 0;

    for (int i = 0; i < alen; i++) {
        cur[curlen++] = a[i];        /* bring down the next digit of the dividend */

        int head = 0;                /* eat leading zeros so length comparisons work */
        while (head < curlen - 1 && cur[head] == '0')
            head++;
        if (head > 0) {
            memmove(cur, cur + head, (size_t)(curlen - head));
            curlen -= head;
        }

        /* Try digits from 9 downwards; the first one that fits is the answer */
        int qd = 0;
        for (int t = 9; t >= 1; t--) {
            int plen = mulSmall(b, blen, t, prod);
            if (cmpDigits(prod, plen, cur, curlen) <= 0) {
                qd = t;
                break;
            }
        }
        if (qd > 0) {
            int plen = mulSmall(b, blen, qd, prod);
            curlen = subAbs(cur, curlen, prod, plen, cur);   /* remainder -= divisor*digit */
        }
        quot[qn++] = (char)('0' + qd);
    }

    int head = 0;                    /* the quotient may have padding zeros */
    while (head < qn - 1 && quot[head] == '0')
        head++;
    if (head > 0) {
        memmove(quot, quot + head, (size_t)(qn - head));
        qn -= head;
    }
    *qlen = qn;

    if (curlen == 0) {               /* unreachable in practice, just in case */
        rem[0] = '0';
        *rlen = 1;
    } else {
        memcpy(rem, cur, (size_t)curlen);
        *rlen = curlen;
    }

    free(cur);
    free(prod);
}

/* ============ arithmetic at the Big level ============ */

/* Pad a digit string out to `scale` decimals, i.e. multiply by a power of ten;
   returns a fresh string */
static char *alignDigits(const char *d, int len, int dscale, int scale, int *outlen)
{
    int extra = scale - dscale;
    char *p = xmalloc((size_t)(len + extra));
    memcpy(p, d, (size_t)len);
    if (extra > 0)
        memset(p + len, '0', (size_t)extra);
    *outlen = len + extra;
    return p;
}

/* Addition and subtraction: align the decimal points first, then add for equal
   signs and subtract for opposite ones. out may overlap a or b */
static void bigAdd(const Big *a, const Big *b, Big *out)
{
    int scale = a->scale > b->scale ? a->scale : b->scale;
    int alen, blen;
    char *ad = alignDigits(a->d, a->len, a->scale, scale, &alen);
    char *bd = alignDigits(b->d, b->len, b->scale, scale, &blen);
    char *res = xmalloc((size_t)(alen > blen ? alen : blen) + 1);
    int rlen, rsign;

    if (a->sign == b->sign) {
        rlen = addAbs(ad, alen, bd, blen, res);
        rsign = a->sign;
    } else {
        int cmp = cmpDigits(ad, alen, bd, blen);
        if (cmp == 0) {                 /* opposite signs cancel out to 0 */
            free(ad);
            free(bd);
            free(res);
            bigSetZero(out);
            return;
        }
        if (cmp > 0) {
            rlen = subAbs(ad, alen, bd, blen, res);
            rsign = a->sign;
        } else {
            rlen = subAbs(bd, blen, ad, alen, res);
            rsign = b->sign;
        }
    }

    free(ad);
    free(bd);
    bigTake(out, res, rlen, scale, rsign);
}

/* Multiplication: multiply the digit strings, add the scales, multiply the signs */
static void bigMul(const Big *a, const Big *b, Big *out)
{
    char *res = xmalloc((size_t)(a->len + b->len) + 1);
    int rlen = mulAbs(a->d, a->len, b->d, b->len, res);
    bigTake(out, res, rlen, a->scale + b->scale, a->sign * b->sign);
}

/*
 * Division: keep DIV_SCALE decimals and round the last one.
 * a/b = (A * 10^(sb + DIV_SCALE)) / (B * 10^sa), then put the point back.
 * A and B are the digit strings, sa and sb the numbers of decimals.
 */
static const char *bigDiv(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        bigSetZero(out);
        return "division by zero";
    }

    int shift = b->scale + DIV_SCALE - a->scale;
    int nlen = a->len, dlen = b->len;
    char *num = xmalloc((size_t)(nlen + (shift > 0 ? shift : 0)));
    char *den = xmalloc((size_t)(dlen + (shift < 0 ? -shift : 0)));

    memcpy(num, a->d, (size_t)nlen);
    memcpy(den, b->d, (size_t)dlen);
    if (shift > 0) {                   /* pad the numerator, i.e. shift right */
        memset(num + nlen, '0', (size_t)shift);
        nlen += shift;
    } else if (shift < 0) {            /* otherwise pad the denominator */
        memset(den + dlen, '0', (size_t)(-shift));
        dlen += -shift;
    }

    char *quot = xmalloc((size_t)nlen + 1);
    char *rem  = xmalloc((size_t)dlen + 2);
    int qlen = 0, rlen = 0;
    divmodAbs(num, nlen, den, dlen, quot, &qlen, rem, &rlen);
    free(num);

    /* Round up when twice the remainder reaches the divisor */
    char *twice = xmalloc((size_t)rlen + 1);
    int tlen = mulSmall(rem, rlen, 2, twice);
    if (cmpDigits(twice, tlen, den, dlen) >= 0) {
        char *q2 = xmalloc((size_t)qlen + 1);
        int q2len = addAbs(quot, qlen, "1", 1, q2);
        free(quot);
        quot = q2;
        qlen = q2len;
    }

    free(twice);
    free(rem);
    free(den);

    bigTake(out, quot, qlen, DIV_SCALE, a->sign * b->sign);   /* trailing zeros trimmed */
    return NULL;
}

/* Modulo: matches fmod, the sign follows the dividend. Once both decimal points
   are aligned this is an integer remainder, so no real division is needed */
static const char *bigMod(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        bigSetZero(out);
        return "division by zero";
    }

    int scale = a->scale > b->scale ? a->scale : b->scale;
    int alen, blen;
    char *ad = alignDigits(a->d, a->len, a->scale, scale, &alen);
    char *bd = alignDigits(b->d, b->len, b->scale, scale, &blen);

    char *quot = xmalloc((size_t)alen + 1);
    char *rem  = xmalloc((size_t)blen + 2);
    int qlen = 0, rlen = 0;
    divmodAbs(ad, alen, bd, blen, quot, &qlen, rem, &rlen);

    free(ad);
    free(bd);
    free(quot);

    bigTake(out, rem, rlen, scale, a->sign);
    return NULL;
}

/* Halve a digit string in place, used by exponentiation; requires scale == 0 */
static void bigHalve(Big *x)
{
    int carry = 0;
    for (int i = 0; i < x->len; i++) {
        int v = carry * 10 + (x->d[i] - '0');
        x->d[i] = (char)('0' + v / 2);
        carry = v % 2;
    }
    bigTrim(x);
}

/*
 * Power: the exponent must be an integer. Binary exponentiation by squaring.
 * A negative exponent means "take the reciprocal".
 * The exponent can be arbitrarily large, hence a Big counter halved digitwise.
 */
static const char *bigPow(const Big *base, const Big *exp, Big *out)
{
    if (exp->scale != 0)
        return "exponent must be an integer";
    if (exp->sign < 0 && bigIsZero(base))
        return "0 to a negative power is undefined";

    /* Bases 0, 1 and -1 do not depend on the exponent's magnitude, handle now */
    if (bigIsZero(base)) {
        bigSetZero(out);
        return NULL;
    }
    if (base->len == 1 && base->scale == 0 && base->d[0] == '1') {
        bigSetInt(out, 1);
        if (base->sign < 0 && (exp->d[exp->len - 1] - '0') % 2 == 1)
            out->sign = -1;
        return NULL;
    }

    /*
     * Estimate the result size: base^exp = M^exp / 10^(scale*exp), so the digit
     * count stays below exp * max(len, scale).
     * No artificial cap here - how far we get depends on memory. The one thing
     * that must be stopped is a digit count an int cannot even express (and
     * which could never be allocated): report "out of memory" instead of
     * spinning forever.
     */
    long long unit = base->len > base->scale ? base->len : base->scale;
    if (unit < 1)
        unit = 1;
    long long e = 0;
    for (int i = 0; i < exp->len; i++) {
        if (e <= INT_MAX)              /* beyond this it is simply "too big" */
            e = e * 10 + (exp->d[i] - '0');
    }
    if (e > (long long)INT_MAX / unit)
        return "out of memory";

    Big result, acc, counter;
    bigInit(&result);
    bigInit(&acc);
    bigInit(&counter);
    bigSetInt(&result, 1);
    bigAssign(&acc, base);
    bigAssign(&counter, exp);
    counter.sign = 1;

    while (!bigIsZero(&counter)) {     /* binary powering: multiply on odd, halve */
        if ((counter.d[counter.len - 1] - '0') & 1)
            bigMul(&result, &acc, &result);
        bigHalve(&counter);
        if (!bigIsZero(&counter))
            bigMul(&acc, &acc, &acc);  /* square the base */
    }

    const char *err = NULL;
    if (exp->sign < 0) {               /* negative exponent, e.g. 2^-3 = 1/8 */
        Big one;
        bigInit(&one);
        bigSetInt(&one, 1);
        err = bigDiv(&one, &result, out);
        bigFree(&one);
    } else {
        bigAssign(out, &result);
    }

    bigFree(&result);
    bigFree(&acc);
    bigFree(&counter);
    return err;
}

/* Render as a string: integer part, point, fraction. Integers print without .0 */
static char *bigFormat(const Big *b)
{
    int intLen = b->len - b->scale;    /* digits in the integer part, may be negative,
                                          e.g. 0.05 */
    size_t cap = (size_t)b->len + (size_t)(b->scale > 0 ? b->scale : 0) + 4;
    char *s = xmalloc(cap);
    size_t k = 0;

    if (b->sign < 0)
        s[k++] = '-';

    if (intLen > 0) {
        memcpy(s + k, b->d, (size_t)intLen);
        k += (size_t)intLen;
    } else {
        s[k++] = '0';                  /* the integer part is 0 */
    }

    if (b->scale > 0) {
        s[k++] = '.';
        if (intLen < 0) {              /* pad with zeros when intLen is negative */
            memset(s + k, '0', (size_t)(-intLen));
            k += (size_t)(-intLen);
        }
        int from = intLen > 0 ? intLen : 0;
        memcpy(s + k, b->d + from, (size_t)(b->len - from));
        k += (size_t)(b->len - from);
    }

    s[k] = '\0';
    return s;
}

/* ======== parse context: carries the message and the offending offset ======== */

typedef struct {
    const char *start;      /* beginning of the whole expression, for offsets */
    const char *cur;        /* scan position */
    const char *err;        /* message, NULL means all good so far */
    int         errPos;     /* how far from start the error sits */
} Ctx;

static void setErr(Ctx *c, const char *pos, const char *msg)
{
    if (c->err == NULL) {              /* keep the first one only, later errors
                                          must not overwrite it */
        c->err = msg;
        c->errPos = (int)(pos - c->start);
    }
}

/* Read a number from the text; where it ended is written back to the cursor */
static void bigFromText(Ctx *c, Big *out)
{
    const char *s = c->cur;
    skipBlank(&s);
    const char *numStart = s;

    int sign = 1;
    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        sign = -1;
        s++;
    }

    bigSetZero(out);
    int digits = 0, scale = 0, sawDot = 0;

    while (isdigit((unsigned char)*s)) {          /* integer part */
        bigReserve(out, digits + 1);
        out->d[digits++] = *s++;
    }
    if (*s == '.') {                               /* fraction part */
        sawDot = 1;
        s++;
        while (isdigit((unsigned char)*s)) {
            bigReserve(out, digits + 1);
            out->d[digits++] = *s++;
            scale++;
        }
    }

    if (digits == 0) {                             /* not a single digit read */
        bigSetZero(out);
        if (isalpha((unsigned char)*s))
            setErr(c, numStart, "functions and variables are not supported");
        else if (sawDot)
            setErr(c, numStart, "missing digits around the decimal point");
        else
            setErr(c, numStart, "missing operand");
        c->cur = s;
        return;
    }

    /* Scientific notation: 1e3, 1.5e-2 */
    long long expo = 0;
    const char *expPos = s;
    if (*s == 'e' || *s == 'E') {
        const char *p = s + 1;
        int esign = 1;
        if (*p == '+' || *p == '-') {
            esign = (*p == '-') ? -1 : 1;
            p++;
        }
        if (isdigit((unsigned char)*p)) {
            while (isdigit((unsigned char)*p)) {
                if (expo < 100000000000000000LL)  /* so long long itself cannot overflow */
                    expo = expo * 10 + (*p - '0');
                p++;
            }
            if (esign < 0)
                expo = -expo;
            s = p;
        }
    }

    /* New decimal count = old count - exponent. long long keeps it from overflowing */
    long long newScale = (long long)scale - expo;

    if (newScale < 0) {
        /* A negative count means a positive exponent, e.g. 1e3: pad zeros to
           move the point back */
        long long need = (long long)digits - newScale;
        if (need > INT_MAX) {          /* too many digits to even store the count */
            setErr(c, expPos, "out of memory");
            bigSetZero(out);
            c->cur = s;
            return;
        }
        bigReserve(out, (int)need);
        memset(out->d + digits, '0', (size_t)(need - digits));
        digits = (int)need;
        scale = 0;
    } else {
        if (newScale > INT_MAX) {      /* this many decimals is likewise unstorable */
            setErr(c, expPos, "out of memory");
            bigSetZero(out);
            c->cur = s;
            return;
        }
        scale = (int)newScale;
    }

    out->len = digits;
    out->scale = scale;
    out->sign = sign;
    bigTrim(out);
    c->cur = s;
}

/* ============ recursive descent parsing ============ */

/* These two call each other, so introduce them first */
static void parseUnarySign(Ctx *c, Big *out);
static void parseAddSub(Ctx *c, Big *out);

/* One operand: a number, or a parenthesised expression */
static void parseOperand(Ctx *c, Big *out)
{
    skipBlank(&c->cur);
    if (*c->cur == '(') {
        c->cur++;                              /* consume the '(' */
        parseAddSub(c, out);                   /* recurse: the inside is a full expression */
        skipBlank(&c->cur);
        if (*c->cur == ')')
            c->cur++;                          /* consume the ')' */
        else
            setErr(c, c->cur, "missing right parenthesis");   /* the caret points at
                                                                 exactly where it is missing */
        return;
    }
    bigFromText(c, out);                       /* anything else is read as a number */
}

/* Power, right associative */
static void parsePower(Ctx *c, Big *out)
{
    Big base, exp;
    bigInit(&base);
    bigInit(&exp);

    parseOperand(c, &base);
    skipBlank(&c->cur);
    if (*c->cur != '^') {                      /* no '^', not a power */
        bigAssign(out, &base);
    } else {
        c->cur++;                              /* consume '^' */
        const char *expPos = c->cur;
        parseUnarySign(c, &exp);               /* recurse one level up so 2^-3 parses */
        const char *err = bigPow(&base, &exp, out);
        if (err != NULL)
            setErr(c, expPos, err);
    }

    bigFree(&base);
    bigFree(&exp);
}

/* Unary + and -: 2*-3, --5 and -2^2 all land here */
static void parseUnarySign(Ctx *c, Big *out)
{
    skipBlank(&c->cur);
    if (*c->cur == '+') {
        c->cur++;
        parseUnarySign(c, out);
        return;
    }
    if (*c->cur == '-') {
        c->cur++;
        parseUnarySign(c, out);
        if (!bigIsZero(out))                   /* the sign of 0 makes no difference */
            out->sign = -out->sign;
        return;
    }
    parsePower(c, out);
}

/* Multiplication, division and modulo share one level, left to right */
static void parseMulDivMod(Ctx *c, Big *out)
{
    Big lhs, rhs;
    bigInit(&lhs);
    bigInit(&rhs);
    parseUnarySign(c, &lhs);

    for (;;) {
        skipBlank(&c->cur);
        char op = *c->cur;
        if (op != '*' && op != '/' && op != '%')
            break;                             /* this level is done */
        const char *opPos = c->cur;
        c->cur++;

        parseUnarySign(c, &rhs);
        const char *err = NULL;
        if (op == '*')
            bigMul(&lhs, &rhs, &lhs);
        else if (op == '/')
            err = bigDiv(&lhs, &rhs, &lhs);
        else
            err = bigMod(&lhs, &rhs, &lhs);
        if (err != NULL)
            setErr(c, opPos, err);             /* the caret sits on / or %, so you see
                                                  at a glance which one failed */
    }

    bigAssign(out, &lhs);
    bigFree(&lhs);
    bigFree(&rhs);
}

/* Addition and subtraction, lowest precedence; the whole expression enters here */
static void parseAddSub(Ctx *c, Big *out)
{
    Big lhs, rhs;
    bigInit(&lhs);
    bigInit(&rhs);
    parseMulDivMod(c, &lhs);

    for (;;) {
        skipBlank(&c->cur);
        char op = *c->cur;
        if (op != '+' && op != '-')
            break;
        c->cur++;

        parseMulDivMod(c, &rhs);
        if (op == '-') {                       /* subtraction is adding the opposite */
            if (!bigIsZero(&rhs))
                rhs.sign = -rhs.sign;
        }
        bigAdd(&lhs, &rhs, &lhs);
    }

    bigAssign(out, &lhs);
    bigFree(&lhs);
    bigFree(&rhs);
}

/* ===== the one public entry point: an expression in, a result or a message out ===== */

static int evalExpression(const char *text,
                          char **resultOut,
                          const char **errOut, int *errPos)
{
    Ctx c;
    c.start = text;
    c.cur = text;
    c.err = NULL;
    c.errPos = 0;

    Big v;
    bigInit(&v);
    parseAddSub(&c, &v);

    skipBlank(&c.cur);
    if (c.err == NULL && *c.cur != '\0')
        setErr(&c, c.cur,
               (*c.cur == ')') ? "unexpected right parenthesis"
                               : "unexpected trailing content");

    if (c.err != NULL) {
        *errOut = c.err;
        *errPos = c.errPos;
        bigFree(&v);
        return -1;
    }

    *resultOut = bigFormat(&v);
    bigFree(&v);
    return 0;
}

static void printError(const char *text, const char *msg, int pos)
{
    printf("%s\n", text);
    for (int i = 0; i < pos && text[i] != '\0'; i++)
        putchar(text[i] == '\t' ? '\t' : ' ');
    printf("^ %s\n", msg);
}

/* ============ built-in formulas (the table used by the automatic run) ============ */

typedef struct {
    const char *expr;
    const char *want;       /* expected result string */
    const char *wantErr;    /* expected message; when non-NULL, want is ignored */
    int         wantPos;    /* expected error offset, -1 means do not check */
} TestCase;

static const TestCase kTests[] = {
    /* ---- basic arithmetic ---- */
    { "(27%4-2)*2+1",          "3",     NULL, 0 },
    { "3.5 * 2",               "7",     NULL, 0 },
    { "0.1 + 0.2",             "0.3",   NULL, 0 },
    { "123.456 + 789.012",     "912.468", NULL, 0 },
    { ".5 + .25",              "0.75",  NULL, 0 },
    { "2 * -3 + 8 / 2",        "-2",    NULL, 0 },
    { "2*-3",                  "-6",    NULL, 0 },
    { "--5",                   "5",     NULL, 0 },
    { "-0.00 + 0",             "0",     NULL, 0 },
    { "1e2",                   "100",   NULL, 0 },
    { "1e-3 * 1000",           "1",     NULL, 0 },

    /* ---- division and precision ---- */
    { "1 / 2",                 "0.5",   NULL, 0 },
    { "1 / 8",                 "0.125", NULL, 0 },
    { "(1/2) * (1/2)",         "0.25",  NULL, 0 },
    { "1 / 3",                 "0." "3333333333" "3333333333", NULL, 0 },
    { "2 / 3",                 "0." "6666666666" "666666666" "7", NULL, 0 },
    { "1 / 16",                "0.0625", NULL, 0 },

    /* ---- modulo ---- */
    { "10 % 3.5",              "3",     NULL, 0 },
    { "10.5 % 3",              "1.5",   NULL, 0 },
    { "-7 % 3",                "-1",    NULL, 0 },
    { "7 % -3",                "1",     NULL, 0 },
    { "1e30 % 7",              "1",     NULL, 0 },
    { "10 ^ 50 % 9",           "1",     NULL, 0 },
    { "(1/3) % (1/3)",         "0",     NULL, 0 },
    { "1 % (1/3)",             "0." "0000000000" "000000000" "1", NULL, 0 },

    /* ---- powers ---- */
    { "2 ^ 3 ^ 2",             "512",   NULL, 0 },
    { "-2 ^ 2",                "-4",    NULL, 0 },
    { "(-2)^2",                "4",     NULL, 0 },
    { "2 ^ -3",                "0.125", NULL, 0 },
    { "2^-3",                  "0.125", NULL, 0 },
    { "0.5 ^ -2",              "4",     NULL, 0 },
    { "2 ^ 0",                 "1",     NULL, 0 },
    { "0 ^ 0",                 "1",     NULL, 0 },
    { "1.5 ^ 2",               "2.25",  NULL, 0 },
    { "2 ^ 64",                "18446744073709551616", NULL, 0 },
    { "2 ^ 100",               "1267650600228229401496703205376", NULL, 0 },

    /* ---- big numbers: what the old digit cap used to reject ---- */
    { "123456789 * 987654321", "121932631112635269", NULL, 0 },
    { "999999999999999999 ^ 2", "999999999999999998000000000000000001", NULL, 0 },
    { "10 ^ 300 - 10 ^ 300 + 1", "1",   NULL, 0 },
    { "2 ^ 1000 / 2 ^ 999",    "2",     NULL, 0 },
    { "2 ^ 10000 % 9",         "7",     NULL, 0 },
    { "(10^50 + 1) ^ 2 - 10^100 - 2 * 10^50", "1", NULL, 0 },

    /* ---- diagnostics and their positions ---- */
    { "1 / 0",                 NULL, "division by zero", 2 },
    { "1 % 0",                 NULL, "division by zero", 2 },
    { "(1+2",                  NULL, "missing right parenthesis", 4 },
    { "1 + (2 * 3",            NULL, "missing right parenthesis", 10 },
    { "1+2)",                  NULL, "unexpected right parenthesis", 3 },
    { "2 +",                   NULL, "missing operand", 3 },
    { "2 *",                   NULL, "missing operand", 3 },
    { "2 3",                   NULL, "unexpected trailing content", 2 },
    { ".",                     NULL, "missing digits around the decimal point", 0 },
    { "2 * x",                 NULL, "functions and variables are not supported", 4 },
    { "2 ^ 0.5",               NULL, "exponent must be an integer", 4 },
    { "0 ^ -1",                NULL, "0 to a negative power is undefined", 4 },
    /* a digit count an int cannot express: genuinely too big, not an arbitrary limit */
    { "2 ^ 999999999999999999", NULL, "out of memory", 4 },
    { "1e9999999999",          NULL, "out of memory", 1 }
    /* Note: things like 1e99999999 no longer error out - they really do go and
       build a billion-digit number, so they are kept out of the automatic list
       to avoid eating all the memory at startup */
};

static int runTests(void)
{
    int total = (int)(sizeof kTests / sizeof *kTests);
    int fail = 0;

    for (int i = 0; i < total; i++) {
        const TestCase *t = &kTests[i];
        char *result = NULL;
        const char *err = NULL;
        int pos = 0;

        int rc = evalExpression(t->expr, &result, &err, &pos);
        int ok;
        if (t->wantErr != NULL) {
            ok = (rc != 0) && err != NULL &&
                 strcmp(err, t->wantErr) == 0 &&
                 (t->wantPos < 0 || pos == t->wantPos);
        } else {
            ok = (rc == 0) && result != NULL && strcmp(result, t->want) == 0;
        }

        if (ok) {
            printf("PASS  %s\n", t->expr);
        } else {
            fail++;
            printf("FAIL  %s\n", t->expr);
            if (t->wantErr != NULL)
                printf("      expected: %s (offset %d)\n", t->wantErr, t->wantPos);
            else
                printf("      expected: %s\n", t->want);
            if (rc == 0)
                printf("      actual:   %s\n", result);
            else
                printf("      actual:   %s (offset %d)\n", err, pos);
        }
        free(result);
    }

    printf("\n%d tests, %d passed, %d failed\n", total, total - fail, fail);
    return fail == 0 ? 0 : 1;
}

/* ============ reading standard input line by line ============ */

static char *readLine(FILE *fp)
{
    size_t cap = 128, len = 0;
    char *buf = xmalloc(cap);
    int ch;

    while ((ch = fgetc(fp)) != EOF && ch != '\n') {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = (char)ch;
    }
    if (ch == EOF && len == 0) {
        free(buf);
        return NULL;
    }
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
                       buf[len - 1] == ' '  || buf[len - 1] == '\t'))
        len--;                          /* tolerate CRLF, trim trailing blanks */
    buf[len] = '\0';
    return buf;
}

/* ============ automatic built-in run plus manual mode ============ */

/* Run every built-in formula: print the result, or the error message */
static void runBuiltIn(void)
{
    int total = (int)(sizeof kTests / sizeof *kTests);
    int okCount = 0;

    printf("============ running built-in formulas (%d) ============\n", total);
    for (int i = 0; i < total; i++) {
        const char *expr = kTests[i].expr;
        char *result = NULL;
        const char *err = NULL;
        int pos = 0;

        if (evalExpression(expr, &result, &err, &pos) == 0) {
            printf("  %s = %s\n", expr, result);
            free(result);
            okCount++;
        } else {
            printf("  %s   -> %s (character %d)\n", expr, err, pos);
        }
    }
    printf("============ total %d: %d evaluated, %d errored ============\n",
           total, okCount, total - okCount);
}

/* Words that end the manual session */
static int isQuitWord(const char *s)
{
    return strcmp(s, "q") == 0 || strcmp(s, "Q") == 0 ||
           strcmp(s, "quit") == 0 || strcmp(s, "exit") == 0 ||
           strcmp(s, "end") == 0;
}

/*
 * Manual mode: one expression per line, print the result.
 * With interactive non-zero a prompt is printed and an empty line or q quits;
 * otherwise (pipe/redirect) empty lines and lines starting with # are skipped,
 * matching the long-standing behaviour.
 */
static void manualLoop(int interactive)
{
    if (interactive)
        printf("\nManual mode: one expression per line, type q or press Enter to finish.\n");

    for (;;) {
        if (interactive) {
            printf("expr> ");
            fflush(stdout);
        }

        char *line = readLine(stdin);
        if (line == NULL)                    /* input exhausted */
            break;

        const char *q = line;
        skipBlank(&q);
        if (*q == '#') {                     /* comment lines are skipped */
            free(line);
            continue;
        }
        if (interactive && (*q == '\0' || isQuitWord(q))) {
            free(line);
            break;
        }
        if (!interactive && *q == '\0') {    /* skip blank lines from a pipe */
            free(line);
            continue;
        }

        char *result = NULL;
        const char *err = NULL;
        int pos = 0;
        if (evalExpression(line, &result, &err, &pos) == 0) {
            printf("%s = %s\n", line, result);
            free(result);
        } else {
            printError(line, err, pos);
        }
        free(line);
    }

    if (interactive)
        printf("Manual mode finished.\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return runTests();

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [--test|--help]\n"
               "Default: run the built-in formulas once, then ask whether to evaluate\n"
               "expressions by hand. Manual mode takes one expression per line, type q\n"
               "or press Enter to finish; lines starting with # are comments.\n"
               "When stdin is not a terminal (pipe, redirect) the question is skipped\n"
               "and lines are read directly.\n",
               argv[0]);
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "Unknown option: %s (try --help)\n", argv[1]);
        return 2;
    }

    runBuiltIn();                  /* start by running every built-in formula */

    if (!STDIN_IS_TTY()) {         /* piped or redirected: no questions, just work */
        manualLoop(0);
        return 0;
    }

    for (;;) {
        printf("\nEvaluate expressions manually? (y = start, n = finish): ");
        fflush(stdout);

        char *ans = readLine(stdin);
        if (ans == NULL) {         /* input exhausted, treat it as finish */
            printf("\n");
            break;
        }

        int yes = (ans[0] == 'y' || ans[0] == 'Y');
        int no  = (ans[0] == '\0' || ans[0] == 'n' || ans[0] == 'N' ||
                   isQuitWord(ans));
        free(ans);

        if (yes) {
            manualLoop(1);
        } else if (no) {
            printf("Done.\n");
            break;
        } else {
            printf("Didn't catch that, please enter y or n.\n");
        }
    }
    return 0;
}
