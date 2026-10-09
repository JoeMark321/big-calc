/*
 * 大数表达式求值器
 *
 * 用法
 *     ./calc               先自动跑一遍内置公式，然后问你要不要手动输入检测
 *     ./calc --test        跑内置测试（带 PASS/FAIL 判定）
 *     ./calc --help        看说明
 *
 * 支持 + - * / % ^ 和括号，数字可以写成 3.14、.5、1e3、2.5E-4 这几种样子。
 * 一个数在内存里长这样：
 *     d     数字串，d[0] 是最高位，中间不带小数点（堆上分配，想多长有多长）
 *     len   数字串有效长度
 *     scale 小数点后有几位，真值 = 数字串当整数看 / 10^scale
 *     sign  1 正、-1 负
 *     比如 -3.14 存成 sign=-1, d="314", len=3, scale=2
 *
 * 优先级从低到高：加减 < 乘除取模 < 一元正负号 < 幂
 * 幂是右结合的：2^3^2 按 2^(3^2) 算；-2^2 按 -(2^2) 算
 * 解析用递归下降，一层函数管一级优先级，加运算符不用动主流程。
 *
 * 几条语义约定（用之前心里有数）
 *   - 除法保留 DIV_SCALE 位小数，末位四舍五入，尾 0 去掉：1/2 打印 0.5
 *   - 取模跟 C 的 fmod 一致，符号跟着被除数走：-7 % 3 = -1
 *     算法是"把两边小数点对齐后取整数余数"。如果两边都是除法截断出来的
 *     （比如 1/3），结果就跟 DIV_SCALE 有关：
 *         (1/3) % (1/3) = 0
 *         1 % (1/3)     = 0.00000000000000000001
 *   - 指数必须是整数：2^0.5 是无理数，"有限小数"这种存法存不下
 *     指数大小不限，2^1000000 能算，就是要等
 *   - 0^0 = 1；0 的负次方报错
 *   - 数字大小没有任何硬上限，SIZE_LIMIT 那道保险已经拿掉。能算多大全看
 *     机器内存：装得下就一直算，真装不下（malloc 失败，或者位数多到 int
 *     都表示不了）才报"内存不够了"——那是物理限制，不是数学限制
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L     /* 让 isatty 在 -std=c99 下也有声明 */
#endif

#if defined(_WIN32)
#include <io.h>
#define STDIN_IS_TTY()  _isatty(0)  /* 标准输入是不是终端 */
#else
#include <unistd.h>
#define STDIN_IS_TTY()  isatty(0)
#endif

#define DIV_SCALE  20               /* 除法保留几位小数，末位四舍五入 */

/* ================= 小工具 ================= */

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n > 0 ? n : 1);
    if (p == NULL)
        die("内存不够了");
    return p;
}

static void *xrealloc(void *old, size_t n)
{
    void *p = realloc(old, n > 0 ? n : 1);
    if (p == NULL)
        die("内存不够了");
    return p;
}

/* 游标往后推，跳过空格。cursor 是"指向扫描位置的指针"，要改外面那个指针只能传地址 */
static void skipBlank(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' ||
           **cursor == '\n' || **cursor == '\r')
        (*cursor)++;
}

/* ================= 大数本体 ================= */

typedef struct {
    char *d;      /* 数字串，d[0] 是最高位，中间不带小数点 */
    int   len;    /* 有效位数 */
    int   cap;    /* d 一共多大，不够就 realloc */
    int   scale;  /* 小数位数 */
    int   sign;   /* 正负 */
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
        if (cap > INT_MAX / 2) {    /* 再翻倍就溢出了，直接要 n 那么大 */
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

/* 用小整数造个数出来，初始化用 */
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

/* 深拷贝。Big 里是堆指针，直接 struct 赋值会两把钥匙开一把锁 */
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

/* 归一化：去掉小数点后的尾 0、整数的前导零，全 0 就规整成 0 */
static void bigTrim(Big *b)
{
    while (b->scale > 0 && b->len > 1 && b->d[b->len - 1] == '0') {
        b->len--;
        b->scale--;                  /* 尾 0 在小数点后，去掉它数值不变 */
    }

    int head = 0;
    while (head < b->len - 1 && b->d[head] == '0')
        head++;
    if (head > 0) {
        memmove(b->d, b->d + head, (size_t)(b->len - head));
        b->len -= head;
    }

    if (bigIsZero(b)) {              /* 0 统一写成 0，别搞出 0.00 这种 */
        b->scale = 0;
        b->sign = 1;
    }
}

/* 把一块现成的数字串直接搬进来，省一次拷贝 */
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

/* ============ 纯数字串层面的运算（不带符号、不带小数点） ============ */

/* 比较两个数字串大小，正数表示 a 大、负数表示 b 大、0 是相等 */
static int cmpDigits(const char *a, int alen, const char *b, int blen)
{
    if (alen != blen)          /* 位数多的大，前提是前导零都去掉了 */
        return alen > blen ? 1 : -1;
    int r = memcmp(a, b, (size_t)alen);
    return r > 0 ? 1 : (r < 0 ? -1 : 0);
}

/* 数字串相加，结果写进 out（容量至少 max(alen,blen)+1），返回结果位数。
   out 不能跟 a、b 重叠。从低位往高位算，跟列竖式一样 */
static int addAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    int n = alen > blen ? alen : blen;
    int carry = 0;

    for (int i = 0; i < n; i++) {
        int da = (i < alen) ? a[alen - 1 - i] - '0' : 0;   /* 从个位那头取数字 */
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        int sum = da + db + carry;
        out[n - 1 - i] = (char)('0' + sum % 10);
        carry = sum / 10;
    }
    if (carry) {                     /* 最后还有进位就多出一位 */
        memmove(out + 1, out, (size_t)n);
        out[0] = (char)('0' + carry);
        n++;
    }
    return n;
}

/* 数字串相减，调用前保证 a >= b，结果写进 out（容量至少 alen），返回位数 */
static int subAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    int borrow = 0;

    for (int i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0' - borrow;
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        if (da < db) {               /* 不够减就向高位借一位 */
            da += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[alen - 1 - i] = (char)('0' + da - db);
    }

    int head = 0;                    /* 借位可能在高位留下 0，去掉 */
    while (head < alen - 1 && out[head] == '0')
        head++;
    if (head > 0) {
        memmove(out, out + head, (size_t)(alen - head));
        alen -= head;
    }
    return alen;
}

/* 数字串乘一位数（0~9），结果写进 out（容量至少 alen+1），返回位数 */
static int mulSmall(const char *a, int alen, int m, char *out)
{
    int carry = 0;

    for (int i = alen - 1; i >= 0; i--) {
        int v = (a[i] - '0') * m + carry;
        out[i] = (char)('0' + v % 10);
        carry = v / 10;
    }
    if (carry) {                     /* 进位顶出新的一位 */
        memmove(out + 1, out, (size_t)alen);
        out[0] = (char)('0' + carry);
        return alen + 1;
    }
    return alen;
}

/* 数字串乘数字串，就是小学学的那个乘法竖式。
   out 容量至少 alen+blen+1，不能跟 a、b 重叠 */
static int mulAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    size_t cells = (size_t)(alen + blen + 2);
    long long *acc = xmalloc(sizeof(long long) * cells);
    memset(acc, 0, sizeof(long long) * cells);

    /* 先把每一位的乘积累加到对应位上（低位在前），暂不进位 */
    for (int i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0';
        for (int j = 0; j < blen; j++)
            acc[i + j] += (long long)da * (b[blen - 1 - j] - '0');
    }

    /* 再统一进位 */
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

    while (n > 1 && acc[n - 1] == 0)   /* 乘出一堆前导零也没意义 */
        n--;
    for (int k = 0; k < n; k++)
        out[k] = (char)('0' + acc[n - 1 - k]);

    free(acc);
    return n;
}

/* 数字串除数字串，商写进 quot，余数写进 rem。逐位试商，就是长除法。
   quot 容量至少 alen+1，rem 容量至少 blen+2 */
static void divmodAbs(const char *a, int alen,
                      const char *b, int blen,
                      char *quot, int *qlen,
                      char *rem,  int *rlen)
{
    char *cur  = xmalloc((size_t)blen + 2);   /* 当前余数 */
    char *prod = xmalloc((size_t)blen + 2);   /* 除数乘上一位商 */
    int curlen = 0, qn = 0;

    for (int i = 0; i < alen; i++) {
        cur[curlen++] = a[i];        /* 余数补一位，把被除数的下一位落下来 */

        int head = 0;                /* 前导零吃掉，免得比大小时被位数干扰 */
        while (head < curlen - 1 && cur[head] == '0')
            head++;
        if (head > 0) {
            memmove(cur, cur + head, (size_t)(curlen - head));
            curlen -= head;
        }

        /* 试商：从 9 往下试，第一个够减的就是这位商 */
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
            curlen = subAbs(cur, curlen, prod, plen, cur);   /* 余数减去除数*商 */
        }
        quot[qn++] = (char)('0' + qd);
    }

    int head = 0;                    /* 商前面可能补了好几个 0，去掉 */
    while (head < qn - 1 && quot[head] == '0')
        head++;
    if (head > 0) {
        memmove(quot, quot + head, (size_t)(qn - head));
        qn -= head;
    }
    *qlen = qn;

    if (curlen == 0) {               /* 正常走不到，保险起见 */
        rem[0] = '0';
        *rlen = 1;
    } else {
        memcpy(rem, cur, (size_t)curlen);
        *rlen = curlen;
    }

    free(cur);
    free(prod);
}

/* ============ 大数层面的四则运算 ============ */

/* 把数字串的小数位补到 scale 位，等于两边同乘 10 的若干次方，返回新串 */
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

/* 加减法：先对齐小数位，然后看符号，一样就加、不一样就减。out 可以跟 a、b 重叠 */
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
        if (cmp == 0) {                 /* 异号相抵，正好是 0 */
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

/* 乘法：数字串相乘，小数位数是两边之和，符号是两边符号相乘 */
static void bigMul(const Big *a, const Big *b, Big *out)
{
    char *res = xmalloc((size_t)(a->len + b->len) + 1);
    int rlen = mulAbs(a->d, a->len, b->d, b->len, res);
    bigTake(out, res, rlen, a->scale + b->scale, a->sign * b->sign);
}

/*
 * 除法：结果保留 DIV_SCALE 位小数，末位四舍五入
 * a/b = (A * 10^(sb + DIV_SCALE)) / (B * 10^sa)，算完再把小数点点回去
 * A、B 是两边的数字串，sa、sb 是两边的小数位数
 */
static const char *bigDiv(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        bigSetZero(out);
        return "除数不能是 0";
    }

    int shift = b->scale + DIV_SCALE - a->scale;
    int nlen = a->len, dlen = b->len;
    char *num = xmalloc((size_t)(nlen + (shift > 0 ? shift : 0)));
    char *den = xmalloc((size_t)(dlen + (shift < 0 ? -shift : 0)));

    memcpy(num, a->d, (size_t)nlen);
    memcpy(den, b->d, (size_t)dlen);
    if (shift > 0) {                   /* 分子补 0，等于小数点右移 */
        memset(num + nlen, '0', (size_t)shift);
        nlen += shift;
    } else if (shift < 0) {            /* 反过来给分母补 0 */
        memset(den + dlen, '0', (size_t)(-shift));
        dlen += -shift;
    }

    char *quot = xmalloc((size_t)nlen + 1);
    char *rem  = xmalloc((size_t)dlen + 2);
    int qlen = 0, rlen = 0;
    divmodAbs(num, nlen, den, dlen, quot, &qlen, rem, &rlen);
    free(num);

    /* 余数的两倍不小于除数就进一位，四舍五入 */
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

    bigTake(out, quot, qlen, DIV_SCALE, a->sign * b->sign);   /* 尾 0 会被 trim 掉 */
    return NULL;
}

/* 取模：跟浮点的 fmod 一致，符号跟着被除数走。
   两边小数点对齐后就是整数取余，不用真的算商 */
static const char *bigMod(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        bigSetZero(out);
        return "除数不能是 0";
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

/* 指数折半用：数字串除以 2，原地改，要求 scale == 0 */
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
 * 幂：指数必须是整数，快速幂，逐位平方累乘。负指数就是求倒数。
 * 指数是多大的整数都行，所以拿 Big 当计数器，一位一位折半。
 */
static const char *bigPow(const Big *base, const Big *exp, Big *out)
{
    if (exp->scale != 0)
        return "指数必须是整数";
    if (exp->sign < 0 && bigIsZero(base))
        return "0 的负次方没有意义";

    /* 底数是 0、1、-1 时结果跟指数大小无关，先处理掉 */
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
     * 估一下结果有多长：base^exp = M^exp / 10^(scale*exp)，
     * 位数不会超过 exp * max(len, scale)。
     * 这里不设人为上限，能算多少全看机器内存；只有一件事必须挡：位数多到
     * int 都表示不了（也根本分配不出来），那就直接说内存不够，别在这儿空转。
     */
    long long unit = base->len > base->scale ? base->len : base->scale;
    if (unit < 1)
        unit = 1;
    long long e = 0;
    for (int i = 0; i < exp->len; i++) {
        if (e <= INT_MAX)              /* 再大也只是"存不下"，不用真读完 */
            e = e * 10 + (exp->d[i] - '0');
    }
    if (e > (long long)INT_MAX / unit)
        return "内存不够了";

    Big result, acc, counter;
    bigInit(&result);
    bigInit(&acc);
    bigInit(&counter);
    bigSetInt(&result, 1);
    bigAssign(&acc, base);
    bigAssign(&counter, exp);
    counter.sign = 1;

    while (!bigIsZero(&counter)) {     /* 快速幂：指数是奇数就乘一次底数，然后指数减半 */
        if ((counter.d[counter.len - 1] - '0') & 1)
            bigMul(&result, &acc, &result);
        bigHalve(&counter);
        if (!bigIsZero(&counter))
            bigMul(&acc, &acc, &acc);  /* 底数自己平方 */
    }

    const char *err = NULL;
    if (exp->sign < 0) {               /* 负指数，比如 2^-3 = 1/8 */
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

/* 打印成字符串：整数部分、小数点、小数部分。整数就直接打整数，不带 .0 */
static char *bigFormat(const Big *b)
{
    int intLen = b->len - b->scale;    /* 整数部分占几位，可能是负数，比如 0.05 */
    size_t cap = (size_t)b->len + (size_t)(b->scale > 0 ? b->scale : 0) + 4;
    char *s = xmalloc(cap);
    size_t k = 0;

    if (b->sign < 0)
        s[k++] = '-';

    if (intLen > 0) {
        memcpy(s + k, b->d, (size_t)intLen);
        k += (size_t)intLen;
    } else {
        s[k++] = '0';                  /* 整数部分是 0 */
    }

    if (b->scale > 0) {
        s[k++] = '.';
        if (intLen < 0) {              /* intLen 为负时先补 0 */
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

/* ============ 解析上下文：出错信息和出错位置都挂在它身上 ============ */

typedef struct {
    const char *start;      /* 整条表达式的开头，用来算偏移 */
    const char *cur;        /* 扫描位置 */
    const char *err;        /* 出错信息，NULL 表示一路顺风 */
    int         errPos;     /* 出错位置离 start 多少字符 */
} Ctx;

static void setErr(Ctx *c, const char *pos, const char *msg)
{
    if (c->err == NULL) {              /* 只记第一条，免得后面的错把前面的盖掉 */
        c->err = msg;
        c->errPos = (int)(pos - c->start);
    }
}

/* 从字符串读一个数，读到哪儿为止写回游标 */
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

    while (isdigit((unsigned char)*s)) {          /* 整数部分 */
        bigReserve(out, digits + 1);
        out->d[digits++] = *s++;
    }
    if (*s == '.') {                               /* 小数部分 */
        sawDot = 1;
        s++;
        while (isdigit((unsigned char)*s)) {
            bigReserve(out, digits + 1);
            out->d[digits++] = *s++;
            scale++;
        }
    }

    if (digits == 0) {                             /* 一个数字都没读到 */
        bigSetZero(out);
        if (isalpha((unsigned char)*s))
            setErr(c, numStart, "不支持函数和变量");
        else if (sawDot)
            setErr(c, numStart, "小数点前后都没有数字");
        else
            setErr(c, numStart, "缺少运算数");
        c->cur = s;
        return;
    }

    /* 科学计数法：1e3、1.5e-2 */
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
                if (expo < 100000000000000000LL)  /* 免得 long long 自己溢出 */
                    expo = expo * 10 + (*p - '0');
                p++;
            }
            if (esign < 0)
                expo = -expo;
            s = p;
        }
    }

    /* 新的小数位数 = 原小数位数 - 指数。用 long long 算，免得中间溢出 */
    long long newScale = (long long)scale - expo;

    if (newScale < 0) {
        /* 小数位数是负的说明指数是正的，比如 1e3，补 0 把小数点挪回去 */
        long long need = (long long)digits - newScale;
        if (need > INT_MAX) {          /* 这么多位连变量都存不下 */
            setErr(c, expPos, "内存不够了");
            bigSetZero(out);
            c->cur = s;
            return;
        }
        bigReserve(out, (int)need);
        memset(out->d + digits, '0', (size_t)(need - digits));
        digits = (int)need;
        scale = 0;
    } else {
        if (newScale > INT_MAX) {      /* 小数点后这么多位同样存不下 */
            setErr(c, expPos, "内存不够了");
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

/* ============ 递归下降解析 ============ */

/* 这俩互相递归，先打个招呼 */
static void parseUnarySign(Ctx *c, Big *out);
static void parseAddSub(Ctx *c, Big *out);

/* 一个操作数：数字，或者括号包起来的表达式 */
static void parseOperand(Ctx *c, Big *out)
{
    skipBlank(&c->cur);
    if (*c->cur == '(') {
        c->cur++;                              /* 吃掉左括号 */
        parseAddSub(c, out);                   /* 括号里当一个完整表达式递归进去 */
        skipBlank(&c->cur);
        if (*c->cur == ')')
            c->cur++;                          /* 吃掉右括号 */
        else
            setErr(c, c->cur, "缺少右括号");   /* 箭头就指在这儿，缺的就是这个位置 */
        return;
    }
    bigFromText(c, out);                       /* 不是括号就当数字读 */
}

/* 幂，右结合 */
static void parsePower(Ctx *c, Big *out)
{
    Big base, exp;
    bigInit(&base);
    bigInit(&exp);

    parseOperand(c, &base);
    skipBlank(&c->cur);
    if (*c->cur != '^') {                      /* 没有 ^ 就不是幂 */
        bigAssign(out, &base);
    } else {
        c->cur++;                              /* 吃掉 ^ */
        const char *expPos = c->cur;
        parseUnarySign(c, &exp);               /* 指数递归回上一层，2^-3 才认得出来 */
        const char *err = bigPow(&base, &exp, out);
        if (err != NULL)
            setErr(c, expPos, err);
    }

    bigFree(&base);
    bigFree(&exp);
}

/* 一元的 + -，2*-3、--5、-2^2 都归它管 */
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
        if (!bigIsZero(out))                   /* 0 的正负没区别 */
            out->sign = -out->sign;
        return;
    }
    parsePower(c, out);
}

/* 乘除取模，同一级优先级，从左往右算 */
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
            break;                             /* 这一层算完了 */
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
            setErr(c, opPos, err);             /* 箭头指在 / 或 % 上，一眼看到是哪个出的事 */
    }

    bigAssign(out, &lhs);
    bigFree(&lhs);
    bigFree(&rhs);
}

/* 加减，优先级最低，整个表达式从这进来 */
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
        if (op == '-') {                       /* 减法就是加上一个相反数 */
            if (!bigIsZero(&rhs))
                rhs.sign = -rhs.sign;
        }
        bigAdd(&lhs, &rhs, &lhs);
    }

    bigAssign(out, &lhs);
    bigFree(&lhs);
    bigFree(&rhs);
}

/* ============ 对外的一条入口：一行表达式进，结果或出错信息出 ============ */

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
               (*c.cur == ')') ? "多余的右括号" : "表达式后面还有多余的内容");

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

/* ============ 内置公式（自动执行用的就是这张表） ============ */

typedef struct {
    const char *expr;
    const char *want;       /* 期望的结果字符串 */
    const char *wantErr;    /* 期望的错误信息，非 NULL 时忽略 want */
    int         wantPos;    /* 期望的出错位置，-1 表示不检查 */
} TestCase;

static const TestCase kTests[] = {
    /* ---- 基本四则 ---- */
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

    /* ---- 除法与精度 ---- */
    { "1 / 2",                 "0.5",   NULL, 0 },
    { "1 / 8",                 "0.125", NULL, 0 },
    { "(1/2) * (1/2)",         "0.25",  NULL, 0 },
    { "1 / 3",                 "0." "3333333333" "3333333333", NULL, 0 },
    { "2 / 3",                 "0." "6666666666" "666666666" "7", NULL, 0 },
    { "1 / 16",                "0.0625", NULL, 0 },

    /* ---- 取模 ---- */
    { "10 % 3.5",              "3",     NULL, 0 },
    { "10.5 % 3",              "1.5",   NULL, 0 },
    { "-7 % 3",                "-1",    NULL, 0 },
    { "7 % -3",                "1",     NULL, 0 },
    { "1e30 % 7",              "1",     NULL, 0 },
    { "10 ^ 50 % 9",           "1",     NULL, 0 },
    { "(1/3) % (1/3)",         "0",     NULL, 0 },
    { "1 % (1/3)",             "0." "0000000000" "000000000" "1", NULL, 0 },

    /* ---- 幂 ---- */
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

    /* ---- 大数：以前有位数上限时会挡下来的 ---- */
    { "123456789 * 987654321", "121932631112635269", NULL, 0 },
    { "999999999999999999 ^ 2", "999999999999999998000000000000000001", NULL, 0 },
    { "10 ^ 300 - 10 ^ 300 + 1", "1",   NULL, 0 },
    { "2 ^ 1000 / 2 ^ 999",    "2",     NULL, 0 },
    { "2 ^ 10000 % 9",         "7",     NULL, 0 },
    { "(10^50 + 1) ^ 2 - 10^100 - 2 * 10^50", "1", NULL, 0 },

    /* ---- 出错提示和位置 ---- */
    { "1 / 0",                 NULL, "除数不能是 0", 2 },
    { "1 % 0",                 NULL, "除数不能是 0", 2 },
    { "(1+2",                  NULL, "缺少右括号", 4 },
    { "1 + (2 * 3",            NULL, "缺少右括号", 10 },
    { "1+2)",                  NULL, "多余的右括号", 3 },
    { "2 +",                   NULL, "缺少运算数", 3 },
    { "2 *",                   NULL, "缺少运算数", 3 },
    { "2 3",                   NULL, "表达式后面还有多余的内容", 2 },
    { ".",                     NULL, "小数点前后都没有数字", 0 },
    { "2 * x",                 NULL, "不支持函数和变量", 4 },
    { "2 ^ 0.5",               NULL, "指数必须是整数", 4 },
    { "0 ^ -1",                NULL, "0 的负次方没有意义", 4 },
    /* 位数多到 int 都表示不了，属于真装不下，不是人为限制 */
    { "2 ^ 999999999999999999", NULL, "内存不够了", 4 },
    { "1e9999999999",          NULL, "内存不够了", 1 }
    /* 说明：1e99999999 这类已经不再报错了，它现在真的会去生成 10 亿位的数，
       所以不放进自动跑的清单，免得一启动就把内存吃光 */
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
                printf("      期望：%s（位置 %d）\n", t->wantErr, t->wantPos);
            else
                printf("      期望：%s\n", t->want);
            if (rc == 0)
                printf("      实际：%s\n", result);
            else
                printf("      实际：%s（位置 %d）\n", err, pos);
        }
        free(result);
    }

    printf("\n%d 条测试，%d 条通过，%d 条失败\n", total, total - fail, fail);
    return fail == 0 ? 0 : 1;
}

/* ============ 一行一行读标准输入 ============ */

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
        len--;                          /* 兼容 CRLF，也把行尾空格收干净 */
    buf[len] = '\0';
    return buf;
}

/* ============ 自动执行内置公式 + 手动检测 ============ */

/* 把内置公式挨个跑一遍：算得出来的打结果，算不出来的打错误 */
static void runBuiltIn(void)
{
    int total = (int)(sizeof kTests / sizeof *kTests);
    int okCount = 0;

    printf("============ 内置公式自动执行（%d 条）============\n", total);
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
            printf("  %s   -> %s（第 %d 个字符）\n", expr, err, pos);
        }
    }
    printf("============ 共 %d 条：%d 条算出结果，%d 条报错 ============\n",
           total, okCount, total - okCount);
}

/* 手动检测时用来收尾的词 */
static int isQuitWord(const char *s)
{
    return strcmp(s, "q") == 0 || strcmp(s, "Q") == 0 ||
           strcmp(s, "quit") == 0 || strcmp(s, "exit") == 0 ||
           strcmp(s, "end") == 0;
}

/*
 * 手动检测：一行一条表达式，算完打出来。
 * interactive 非 0 就打提示符，空行或 q 直接收工；否则（管道/重定向）
 * 空行和 # 开头的行跳过，跟以前的行为一致。
 */
static void manualLoop(int interactive)
{
    if (interactive)
        printf("\n手动检测：一行一条表达式，输入 q 或直接回车结束。\n");

    for (;;) {
        if (interactive) {
            printf("表达式> ");
            fflush(stdout);
        }

        char *line = readLine(stdin);
        if (line == NULL)                    /* 输入到头了 */
            break;

        const char *q = line;
        skipBlank(&q);
        if (*q == '#') {                     /* 注释行直接跳过 */
            free(line);
            continue;
        }
        if (interactive && (*q == '\0' || isQuitWord(q))) {
            free(line);
            break;
        }
        if (!interactive && *q == '\0') {    /* 管道输入时空行跳过 */
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
        printf("手动检测结束。\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return runTests();

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("用法：%s [--test|--help]\n"
               "默认：先把内置公式自动跑一遍，然后问你要不要手动输入检测。\n"
               "手动检测一行一条表达式，输入 q 或直接回车结束；# 开头的行是注释。\n"
               "标准输入不是终端（管道、重定向）时跳过提问，直接逐行读表达式。\n",
               argv[0]);
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "不认识的参数：%s（试试 --help）\n", argv[1]);
        return 2;
    }

    runBuiltIn();                  /* 一上来先把内置公式全跑一遍 */

    if (!STDIN_IS_TTY()) {         /* 管道/重定向进来：别问问题，直接干活 */
        manualLoop(0);
        return 0;
    }

    for (;;) {
        printf("\n要手动输入表达式检测吗？(y=开始检测，n=结束): ");
        fflush(stdout);

        char *ans = readLine(stdin);
        if (ans == NULL) {         /* 输入到头了，就当结束 */
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
            printf("好，到此结束。\n");
            break;
        } else {
            printf("没看懂，请输入 y 或 n。\n");
        }
    }
    return 0;
}
