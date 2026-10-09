#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * 大数表达式求值器
 *
 * 不用 double，数字就存成字符串，想多长就多长。
 * 支持 + - * / % ^ 和括号，数字能写成 3.14、.5、1e3 这几种样子。
 *
 * 一个数在内存里长这样：
 *   d     数字串，d[0] 是最高位，中间不带小数点
 *   len   数字串有效长度
 *   scale 小数点后有几位，真值 = 数字串当整数看 / 10^scale
 *   sign  1 正、-1 负
 *   比如 -3.14 存成 sign=-1, d="314", len=3, scale=2
 *
 * 优先级从低到高：加减 < 乘除取模 < 一元正负号 < 幂
 * 幂是右结合的：2^3^2 按 2^(3^2) 算
 * 解析还是递归下降，一层函数管一级优先级，加运算符不用动主流程。
 */

#define MAX_DIGITS 256                  /* 一个数最多存多少位数字，不够就调大这个 */
#define MAX_TEMP   (MAX_DIGITS * 2 + 8) /* 乘积、移位用的临时缓冲，留点余量 */
#define DIV_SCALE  20                   /* 除法保留几位小数，末位四舍五入 */

typedef struct {
    char d[MAX_DIGITS];   /* 数字串 */
    int  len;             /* 有效位数 */
    int  scale;           /* 小数位数 */
    int  sign;            /* 正负 */
} Big;

/* 出错信息，NULL 表示一路顺风。只记第一条，免得后面的错把前面的盖掉 */
static const char *errorMsg = NULL;

static void setError(const char *msg)
{
    if (errorMsg == NULL)
        errorMsg = msg;
}

/* 游标往后推，跳过空格。cursor 是"指向扫描位置的指针"，要改外面那个指针只能传地址 */
static void skipBlank(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n')
        (*cursor)++;
}

/* ============ 纯数字串层面的运算（不带符号、不带小数点） ============ */

/* 比较两个数字串大小，正数表示 a 大、负数表示 b 大、0 是相等 */
static int cmpDigits(const char *a, int alen, const char *b, int blen)
{
    if (alen != blen)          /* 位数多的大，前提是前导零都去掉了 */
        return alen > blen ? 1 : -1;
    return memcmp(a, b, alen); /* 位数一样就逐位比 */
}

/* 数字串相加，结果写进 out，返回结果位数。从低位往高位算，跟列竖式一样 */
static int addAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    char buf[MAX_TEMP];               /* 先写临时区，最后再倒腾出去，这样 out 和 a、b 重叠也不怕 */
    int n = alen > blen ? alen : blen;
    int carry = 0;
    int i;

    for (i = 0; i < n; i++) {
        int da = (i < alen) ? a[alen - 1 - i] - '0' : 0;   /* 从个位那头取数字 */
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        int sum = da + db + carry;
        buf[i] = (char)('0' + sum % 10);
        carry = sum / 10;
    }
    if (carry)                       /* 最后还有进位就多出一位 */
        buf[i++] = (char)('0' + carry);

    for (int k = 0; k < i; k++)      /* 临时区是倒着存的，抄回去时翻个身 */
        out[k] = buf[i - 1 - k];
    return i;
}

/* 数字串相减，调用前保证 a >= b，结果写进 out，返回结果位数 */
static int subAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    char buf[MAX_TEMP];
    int borrow = 0;
    int i;

    for (i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0' - borrow;
        int db = (i < blen) ? b[blen - 1 - i] - '0' : 0;
        if (da < db) {               /* 不够减就向高位借一位 */
            da += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        buf[i] = (char)('0' + da - db);
    }

    int n = alen;
    while (n > 1 && buf[n - 1] == '0')   /* 借位可能在高位留下 0，去掉 */
        n--;
    for (int k = 0; k < n; k++)
        out[k] = buf[n - 1 - k];
    return n;
}

/* 数字串乘一位数（0~9），结果写进 out */
static int mulSmall(const char *a, int alen, int m, char *out)
{
    char buf[MAX_TEMP];
    int carry = 0;

    for (int i = alen - 1; i >= 0; i--) {
        int v = (a[i] - '0') * m + carry;
        buf[i] = (char)('0' + v % 10);
        carry = v / 10;
    }

    int n = alen;
    if (carry) {                     /* 进位顶出新的一位 */
        memmove(buf + 1, buf, alen);
        buf[0] = (char)('0' + carry);
        n++;
    }
    memcpy(out, buf, n);
    return n;
}

/* 数字串乘数字串，就是小学学的那个乘法竖式 */
static int mulAbs(const char *a, int alen, const char *b, int blen, char *out)
{
    int acc[MAX_TEMP];
    int total = alen + blen;
    memset(acc, 0, sizeof(int) * total);

    /* 先把每一位的乘积累加到对应位上，暂不进位 */
    for (int i = 0; i < alen; i++) {
        int da = a[alen - 1 - i] - '0';
        for (int j = 0; j < blen; j++)
            acc[i + j] += da * (b[blen - 1 - j] - '0');
    }

    /* 再统一进位 */
    int carry = 0;
    int n = total;
    for (int k = 0; k < n; k++) {
        int v = acc[k] + carry;
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
    return n;
}

/* 数字串除数字串，商写进 quot，余数写进 rem。逐位试商，就是长除法 */
static void divmodAbs(const char *a, int alen,
                      const char *b, int blen,
                      char *quot, int *quotLen,
                      char *rem,  int *remLen)
{
    char cur[MAX_TEMP];    /* 当前余数 */
    char prod[MAX_TEMP];   /* 除数乘上一位商 */
    int  curlen = 0;
    int  qn = 0;

    for (int i = 0; i < alen; i++) {
        cur[curlen++] = a[i];        /* 余数补一位，把被除数的下一位落下来 */

        int head = 0;                /* 前导零吃掉，免得比大小时被位数干扰 */
        while (head < curlen - 1 && cur[head] == '0')
            head++;
        if (head > 0) {
            memmove(cur, cur + head, curlen - head);
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
            curlen = subAbs(cur, curlen, prod, plen, cur);   /* 余数减去 除数*商 */
        }
        quot[qn++] = (char)('0' + qd);
    }

    int head = 0;                    /* 商前面可能补了好几个 0，去掉 */
    while (head < qn - 1 && quot[head] == '0')
        head++;
    memmove(quot, quot + head, qn - head);
    *quotLen = qn - head;

    if (curlen == 0) {               /* 余数是 0 */
        rem[0] = '0';
        *remLen = 1;
    } else {
        memcpy(rem, cur, curlen);
        *remLen = curlen;
    }
}

/* ============ 大数层面：装箱、读写、四则运算 ============ */

static void bigSetZero(Big *b)
{
    b->d[0] = '0';
    b->len = 1;
    b->scale = 0;
    b->sign = 1;
}

/* 用小整数造个数出来，初始化用 */
static void bigSetInt(Big *b, int v)
{
    sprintf(b->d, "%d", v);
    b->len = (int)strlen(b->d);
    b->scale = 0;
    b->sign = 1;
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
        memmove(b->d, b->d + head, b->len - head);
        b->len -= head;
    }

    if (bigIsZero(b)) {              /* 0 统一写成 0，别搞出 0.00 这种 */
        b->scale = 0;
        b->sign = 1;
    }
}

/* 把小数位补齐到 scale 位，等于两边同乘 10 的若干次方 */
static void bigAlign(Big *x, int scale)
{
    while (x->scale < scale && x->len < MAX_DIGITS) {
        x->d[x->len++] = '0';
        x->scale++;
    }
    if (x->scale < scale)
        setError("数字太大了，调大 MAX_DIGITS 试试");
}

/* 从字符串读一个数，读到哪儿为止写回 *cursor */
static void bigFromText(const char **cursor, Big *out)
{
    const char *s = *cursor;
    skipBlank(&s);

    out->sign = 1;
    out->len = 0;
    out->scale = 0;

    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        out->sign = -1;
        s++;
    }

    int digits = 0;
    while (isdigit((unsigned char)*s)) {          /* 整数部分 */
        if (digits < MAX_DIGITS)
            out->d[digits++] = *s;
        s++;
    }
    if (*s == '.') {                               /* 小数部分 */
        s++;
        while (isdigit((unsigned char)*s)) {
            if (digits < MAX_DIGITS) {
                out->d[digits++] = *s;
                out->scale++;
            }
            s++;
        }
    }
    out->len = digits;

    if (digits == 0) {                             /* 一个数字都没读到 */
        setError("表达式写错了");
        bigSetZero(out);
        if (*s != '\0')
            s++;                                   /* 好歹往前挪一位，别卡死在原地 */
        *cursor = s;
        return;
    }

    /* 科学计数法：1e3、1.5e-2 */
    if (*s == 'e' || *s == 'E') {
        const char *p = s + 1;
        int esign = 1;
        if (*p == '+' || *p == '-') {
            esign = (*p == '-') ? -1 : 1;
            p++;
        }
        if (isdigit((unsigned char)*p)) {
            long exp = 0;
            while (isdigit((unsigned char)*p)) {
                if (exp < 100000)                  /* 指数大得离谱就不再乘了，免得溢出 */
                    exp = exp * 10 + (*p - '0');
                p++;
            }
            out->scale -= esign * (int)exp;
            s = p;
        }
    }

    /* scale 是负数说明指数是正的，比如 1e3，补 0 把小数点挪回去 */
    while (out->scale < 0 && out->len < MAX_DIGITS) {
        out->d[out->len++] = '0';
        out->scale++;
    }
    if (out->scale < 0)
        setError("数字太大了，调大 MAX_DIGITS 试试");

    bigTrim(out);
    *cursor = s;
}

/* 打印：整数部分、小数点、小数部分。整数就直接打整数，不带 .0 */
static void bigPrint(const Big *b)
{
    int intLen = b->len - b->scale;     /* 整数部分占几位，可能是负数，比如 0.05 */

    if (b->sign < 0)
        putchar('-');

    if (intLen <= 0)
        putchar('0');                   /* 整数部分是 0 */
    else
        for (int i = 0; i < intLen; i++)
            putchar(b->d[i]);

    if (b->scale > 0) {
        putchar('.');
        for (int i = intLen; i < b->len; i++)   /* intLen 为负时先补 0 */
            putchar(i < 0 ? '0' : b->d[i]);
    }
}

/* 加减法：先对齐小数位，然后看符号，一样就加、不一样就减 */
static void bigAdd(const Big *a, const Big *b, Big *out)
{
    Big x = *a, y = *b;                 /* 各存一份，out 跟输入重叠也不怕 */
    int scale = x.scale > y.scale ? x.scale : y.scale;
    bigAlign(&x, scale);
    bigAlign(&y, scale);

    if (x.sign == y.sign) {
        out->len = addAbs(x.d, x.len, y.d, y.len, out->d);
        out->sign = x.sign;
    } else {
        int cmp = cmpDigits(x.d, x.len, y.d, y.len);
        if (cmp == 0) {                 /* 异号相抵，正好是 0 */
            bigSetZero(out);
            return;
        }
        if (cmp > 0) {
            out->len = subAbs(x.d, x.len, y.d, y.len, out->d);
            out->sign = x.sign;
        } else {
            out->len = subAbs(y.d, y.len, x.d, x.len, out->d);
            out->sign = y.sign;
        }
    }
    out->scale = scale;
    bigTrim(out);
}

/* 乘法：数字串相乘，小数位数是两边之和，符号是两边符号相乘 */
static void bigMul(const Big *a, const Big *b, Big *out)
{
    char buf[MAX_TEMP];
    int nlen = mulAbs(a->d, a->len, b->d, b->len, buf);
    int scale = a->scale + b->scale;
    int sign = a->sign * b->sign;

    if (nlen > MAX_DIGITS) {
        setError("数字太大了，调大 MAX_DIGITS 试试");
        bigSetZero(out);
        return;
    }
    memcpy(out->d, buf, nlen);
    out->len = nlen;
    out->scale = scale;
    out->sign = sign;
    bigTrim(out);
}

/*
 * 除法：结果保留 DIV_SCALE 位小数，末位四舍五入
 * a/b = (A * 10^(sb + DIV_SCALE)) / (B * 10^sa)，算完再把小数点点回去
 * A、B 是两边的数字串，sa、sb 是两边的小数位数
 */
static void bigDiv(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        setError("除数是 0");
        bigSetZero(out);
        return;
    }

    char num[MAX_TEMP], den[MAX_TEMP], quot[MAX_TEMP], rem[MAX_TEMP];
    int nlen = a->len, dlen = b->len;
    memcpy(num, a->d, nlen);
    memcpy(den, b->d, dlen);

    int shift = b->scale + DIV_SCALE - a->scale;
    if (shift >= 0) {                   /* 分子补 0，等于小数点右移 */
        if (nlen + shift > MAX_TEMP) {
            setError("数字太大了，调大 MAX_DIGITS 试试");
            bigSetZero(out);
            return;
        }
        memset(num + nlen, '0', shift);
        nlen += shift;
    } else {                            /* 反过来给分母补 0 */
        int extra = -shift;
        if (dlen + extra > MAX_TEMP) {
            setError("数字太大了，调大 MAX_DIGITS 试试");
            bigSetZero(out);
            return;
        }
        memset(den + dlen, '0', extra);
        dlen += extra;
    }

    int qlen = 0, rlen = 0;
    divmodAbs(num, nlen, den, dlen, quot, &qlen, rem, &rlen);

    /* 余数的两倍不小于除数就进一位，四舍五入 */
    char twice[MAX_TEMP];
    int tlen = mulSmall(rem, rlen, 2, twice);
    if (cmpDigits(twice, tlen, den, dlen) >= 0)
        qlen = addAbs(quot, qlen, "1", 1, quot);

    if (qlen > MAX_DIGITS) {
        setError("数字太大了，调大 MAX_DIGITS 试试");
        bigSetZero(out);
        return;
    }
    memcpy(out->d, quot, qlen);
    out->len = qlen;
    out->scale = DIV_SCALE;
    out->sign = (a->sign == b->sign) ? 1 : -1;
    bigTrim(out);                       /* 尾 0 去掉，0.125000… 就成了 0.125 */
}

/*
 * 取模：跟浮点的 fmod 一致，符号跟着被除数走
 * 把两边的小数位对齐后就是整数取余，不用真的算商
 */
static void bigMod(const Big *a, const Big *b, Big *out)
{
    if (bigIsZero(b)) {
        setError("除数是 0");
        bigSetZero(out);
        return;
    }

    Big x = *a, y = *b;
    int scale = x.scale > y.scale ? x.scale : y.scale;
    bigAlign(&x, scale);
    bigAlign(&y, scale);

    char quot[MAX_TEMP], rem[MAX_TEMP];
    int qlen = 0, rlen = 0;
    divmodAbs(x.d, x.len, y.d, y.len, quot, &qlen, rem, &rlen);

    memcpy(out->d, rem, rlen);
    out->len = rlen;
    out->scale = scale;
    out->sign = x.sign;
    bigTrim(out);
}

/* 幂：指数必须是整数，快速幂，逐位平方累乘。负指数就是求倒数 */
static void bigPow(const Big *base, const Big *exp, Big *out)
{
    if (exp->scale != 0 || exp->len > 9) {
        setError("指数得是整数，而且别大到 9 位以上");
        bigSetZero(out);
        return;
    }

    long e = 0;
    for (int i = 0; i < exp->len; i++)
        e = e * 10 + (exp->d[i] - '0');

    Big result, acc;
    bigSetInt(&result, 1);
    acc = *base;

    while (e > 0) {                     /* 快速幂：指数是奇数就乘一次底数，然后指数减半 */
        if (e & 1)
            bigMul(&result, &acc, &result);
        e >>= 1;
        if (e)
            bigMul(&acc, &acc, &acc);   /* 底数自己平方 */
    }

    if (exp->sign < 0) {                /* 负指数，比如 2^-3 = 1/8 */
        Big one;
        bigSetInt(&one, 1);
        bigDiv(&one, &result, out);
    } else {
        *out = result;
    }
}

/* ============ 递归下降解析 ============ */

/* 这俩互相递归，先打个招呼 */
static void parseUnarySign(const char **cursor, Big *out);
static void parseAddSub(const char **cursor, Big *out);

/* 一个操作数：数字，或者括号包起来的表达式 */
static void parseOperand(const char **cursor, Big *out)
{
    skipBlank(cursor);
    if (**cursor == '(') {
        (*cursor)++;                              /* 吃掉左括号 */
        parseAddSub(cursor, out);                 /* 括号里当一个完整表达式递归进去 */
        skipBlank(cursor);
        if (**cursor == ')')
            (*cursor)++;                          /* 吃掉右括号 */
        else
            setError("表达式写错了");             /* 括号没闭合 */
        return;
    }
    bigFromText(cursor, out);                     /* 不是括号就当数字读 */
}

/* 幂，右结合 */
static void parsePower(const char **cursor, Big *out)
{
    Big base, exp;
    parseOperand(cursor, &base);

    skipBlank(cursor);
    if (**cursor != '^') {                        /* 没有 ^ 就不是幂 */
        *out = base;
        return;
    }
    (*cursor)++;                                  /* 吃掉 ^ */
    parseUnarySign(cursor, &exp);                 /* 指数递归回上一层，2^-3 才认得出来 */
    bigPow(&base, &exp, out);
}

/* 一元的 + -，2*-3、--5、-2^2 都归它管 */
static void parseUnarySign(const char **cursor, Big *out)
{
    skipBlank(cursor);
    if (**cursor == '+') {
        (*cursor)++;
        parseUnarySign(cursor, out);
        return;
    }
    if (**cursor == '-') {
        (*cursor)++;
        parseUnarySign(cursor, out);
        if (!bigIsZero(out))                      /* 0 的正负没区别 */
            out->sign = -out->sign;
        return;
    }
    parsePower(cursor, out);
}

/* 乘除取模，同一级优先级，从左往右算 */
static void parseMulDivMod(const char **cursor, Big *out)
{
    Big lhs, rhs, tmp;
    parseUnarySign(cursor, &lhs);

    for (;;) {
        skipBlank(cursor);
        char op = **cursor;
        if (op != '*' && op != '/' && op != '%')
            break;                                /* 这一层算完了 */
        (*cursor)++;

        parseUnarySign(cursor, &rhs);
        if (op == '*')
            bigMul(&lhs, &rhs, &tmp);
        else if (op == '/')
            bigDiv(&lhs, &rhs, &tmp);
        else
            bigMod(&lhs, &rhs, &tmp);
        lhs = tmp;
    }
    *out = lhs;
}

/* 加减，优先级最低，整个表达式从这进来 */
static void parseAddSub(const char **cursor, Big *out)
{
    Big lhs, rhs, tmp;
    parseMulDivMod(cursor, &lhs);

    for (;;) {
        skipBlank(cursor);
        char op = **cursor;
        if (op != '+' && op != '-')
            break;
        (*cursor)++;

        parseMulDivMod(cursor, &rhs);
        if (op == '+')
            bigAdd(&lhs, &rhs, &tmp);
        else {                                    /* 减法就是加上一个相反数 */
            rhs.sign = bigIsZero(&rhs) ? 1 : -rhs.sign;
            bigAdd(&lhs, &rhs, &tmp);
        }
        lhs = tmp;
    }
    *out = lhs;
}

static void printAnswer(const char *text, const Big *answer)
{
    printf("%-34s = ", text);
    if (errorMsg) {
        puts(errorMsg);
        return;
    }
    bigPrint(answer);
    putchar('\n');
}

int main(void)
{
    const char *testList[] = {
        "(27%4-2)*2+1",             /* 3 */
        "3.5 * 2",                  /* 7，有小数参与，结果是整数就按整数显示 */
        "2 ^ 3 ^ 2",                /* 512，右结合，不是 64 */
        "-2 ^ 2",                   /* -4，负号优先级低于幂 */
        "2 ^ -3",                   /* 0.125，负指数就是求倒数 */
        "1 / 3",                    /* 0.333…，除法默认留 20 位小数 */
        "10 % 3.5",                 /* 3，取模支持小数 */
        "0.1 + 0.2",                /* 0.3，大数加法天然精确，没有浮点误差 */
        "2 ^ 100",                  /* 1267650600228229401496703205376 */
        "123456789 * 987654321",    /* 121932631112635269 */
        "999999999999999999 ^ 2",   /* 36 位的平方 */
        "1e30 % 7",                 /* 6，超长数字取模 */
        "123.456 + 789.012",        /* 912.468 */
        ".5 + .25",                 /* 0.75，省略整数部分 */
        "2 * -3 + 8 / 2",           /* -2，乘号后面直接跟负号 */
        "(1/2) * (1/2)",            /* 0.25 */
        "1 / 0",                    /* 除数是 0 */
        "(1+2"                      /* 括号没闭合 */
    };

    int testCount = (int)(sizeof testList / sizeof *testList);
    for (int i = 0; i < testCount; i++) {
        errorMsg = NULL;                          /* 每条表达式重新来，别串味 */

        const char *cursor = testList[i];
        Big answer;
        bigSetZero(&answer);
        parseAddSub(&cursor, &answer);            /* 从最低优先级那层开算 */
        printAnswer(testList[i], &answer);
    }
    return 0;
}
