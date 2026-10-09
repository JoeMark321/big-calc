#include <stdio.h>   /* printf / puts，输出结果用的 */
#include <stdlib.h>  /* strtod，把字符串里的数字一次转成 double */
#include <math.h>    /* pow / fmod / floor / fabs / isfinite，数学函数 */

/*
 * 简易表达式求值器（支持浮点）
 *
 * 支持 + - * / % ^ 和括号，数字能写成 3.14、.5、1e3。
 * 优先级从低到高：加减 < 乘除取模 < 一元正负号 < 幂
 * 幂是右结合的：2^3^2 按 2^(3^2) 算
 *
 * 做法就是递归下降，一层函数管一级优先级。比起一路 while 扫过去，
 * 这样加运算符、改优先级都不用大动干戈。
 */

/* 解析到一半发现表达式写错了就置 1，打印前看一眼，别拿垃圾结果糊弄人 */
static int parseError = 0;   /* 全局的错误标志，0=正常，1=表达式有问题 */

/*
 * skipBlank：把游标往后推，跳过当前位置的空白字符
 * cursor 是"指向当前扫描位置的指针"，写成 const char ** 是因为
 * 函数里要移动外面那个指针，必须传地址进来才能改。
 */
static void skipBlank(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n')  /* 空格、制表、换行都算空白 */
        (*cursor)++;                                                 /* 游标后移一位 */
}

/*
 * 下面两个函数互相递归（幂的指数允许带符号，指数里又可能有幂），
 * 括号里也要递归回最低优先级那层，所以先在这儿打个招呼。
 * 没有这两行声明，编译器在 parsePower 里看到 parseUnarySign 会说"不认识"。
 */
static double parseUnarySign(const char **cursor);   /* 前向声明：一元正负号那层 */
static double parseAddSub(const char **cursor);      /* 前向声明：加减那层（最低优先级） */

/*
 * parseOperand：解析一个"操作数"
 * 操作数只有两种形态：一个数字，或者一对括号包起来的完整表达式。
 */
static double parseOperand(const char **cursor)
{
    skipBlank(cursor);                    /* 先把前面可能的空格吃掉 */

    if (**cursor == '(') {                /* 遇到左括号，说明是个子表达式 */
        (*cursor)++;                      /* 吃掉左括号，游标走到括号内容的第一个字符 */

        /* 括号里面就是个独立的小表达式，递归回最低优先级那层重新走一遍 */
        double innerValue = parseAddSub(cursor);

        skipBlank(cursor);                /* 括号后面也可能有空格 */
        if (**cursor == ')')
            (*cursor)++;                  /* 正常：吃掉右括号 */
        else
            parseError = 1;               /* 括号没闭合，标个错，后面打印时会提示 */
        return innerValue;                /* 把括号里算出来的值返回给上层 */
    }

    /* 不是括号就当数字处理 */
    /*
     * strtod 一次搞定 3.14 / .5 / 1e-3 这些写法：
     * 第一个参数是起始位置，第二个参数会指向它解析到哪儿停的，
     * 手写数字解析纯属找罪受。
     */
    char *endOfNumber = NULL;             /* strtod 会把"读到哪儿为止"写到这里 */
    double value = strtod(*cursor, &endOfNumber);   /* 把当前位置的数字转成 double */

    if (endOfNumber == *cursor) {         /* 一个字符都没读到 = 这位置根本不是数字 */
        parseError = 1;                   /* 标个错 */
        if (**cursor != '\0')             /* 除非已经到字符串结尾 */
            (*cursor)++;                  /* 否则至少往前挪一位，保证解析能继续推进，不然会卡死在这 */
        return 0.0;                       /* 数字没读到，给个 0 兜底 */
    }

    *cursor = endOfNumber;                /* 把游标推进到数字结束的位置 */
    return value;                         /* 返回读到的这个数字 */
}

/*
 * parsePower：幂运算，右结合。
 * 指数那边递归回来还可能再撞上 ^，2^3^2 就是这么变成 2^9 的。
 */
static double parsePower(const char **cursor)
{
    double base = parseOperand(cursor);   /* 先取底数（一个数字或括号表达式） */

    skipBlank(cursor);                    /* 跳过底数后面的空格 */
    if (**cursor != '^')                  /* 没跟 ^ 就说明不是幂运算 */
        return base;                      /* 底数本身就是这层的结果，直接返回 */

    (*cursor)++;                          /* 吃掉 ^ */

    /* 指数这层递归回一元符号那层，2^-3 这种负指数才能认出来 */
    double exponent = parseUnarySign(cursor);

    return pow(base, exponent);           /* 调 pow 算底数的指数次方 */
}

/*
 * parseUnarySign：处理一元的 + 和 -。
 * 2*-3、--5、-2^2 都归它管。
 * 它放在 parsePower 外面，所以 -2^2 的负号是作用在整个幂运算结果上的，得 -4。
 */
static double parseUnarySign(const char **cursor)
{
    skipBlank(cursor);                                                    /* 跳空格 */

    /* 遇到 + 就吃掉它，符号不变，继续往里看还有没有别的符号 */
    if (**cursor == '+') { (*cursor)++; return  parseUnarySign(cursor); }

    /* 遇到 - 就吃掉它，然后把递归回来的结果取反，--5 最后就是正 5 */
    if (**cursor == '-') { (*cursor)++; return -parseUnarySign(cursor); }

    /* 前面没符号，说明是个正经操作数（可能带幂），往下一级走 */
    return parsePower(cursor);
}

/*
 * parseMulDivMod：乘、除、取模，同一级优先级，从左往右算。
 * 比如 12 / 3 * 2 是 (12/3)*2 = 8，不是 12/(3*2)。
 */
static double parseMulDivMod(const char **cursor)
{
    double result = parseUnarySign(cursor);   /* 先取第一个操作数 */

    for (;;) {                                 /* 只要后面还跟着 * / % 就一直算下去 */
        skipBlank(cursor);                     /* 跳空格 */

        char op = **cursor;                    /* 把运算符记下来 */
        if (op != '*' && op != '/' && op != '%')
            break;                             /* 不是这三个就说明这层结束了，交给上层 */

        (*cursor)++;                           /* 吃掉运算符 */

        double rightValue = parseUnarySign(cursor);   /* 取运算符右边那个操作数 */

        if (op == '*') {
            result *= rightValue;              /* 乘法，直接乘 */
        } else if (op == '/') {
            result /= rightValue;              /* 除法，除 0 会得到 inf，最后打印时兜着 */
        } else {
            /*
             * 浮点取模只能用 fmod，% 运算符不认 double。
             * fmod(10, 3.5) = 3，语义跟整数取模一样，符号跟被除数走。
             */
            result = fmod(result, rightValue);
        }
    }
    return result;                             /* 这一串乘除取模算完的结果 */
}

/*
 * parseAddSub：加减，优先级最低，整个表达式从这里进来。
 * 它是最外层，函数调用链：parseAddSub → parseMulDivMod → parseUnarySign → parsePower → parseOperand
 */
static double parseAddSub(const char **cursor)
{
    double result = parseMulDivMod(cursor);    /* 先把乘除取模那一坨算出来 */

    for (;;) {
        skipBlank(cursor);                     /* 跳空格 */

        char op = **cursor;                    /* 记下运算符 */
        if (op != '+' && op != '-')
            break;                             /* 不是加减就结束了，比如遇到 ')' 或字符串结尾 */

        (*cursor)++;                           /* 吃掉 + 或 - */

        double rightValue = parseMulDivMod(cursor);   /* 右边再来一坨乘除取模 */

        /* 按运算符选择加还是减 */
        result = (op == '+') ? result + rightValue
                             : result - rightValue;
    }
    return result;
}

/*
 * printAnswer：打印结果。
 * 核心是那句"整数就别带小数点"：正好是整数按整数打，否则按浮点打，
 * 不会冒出 7.000000 这种尾巴。
 */
static void printAnswer(const char *expressionText, double answer)
{
    printf("%-30s = ", expressionText);   /* 先把原表达式左对齐打出来，宽度 30，看着整齐 */

    if (parseError) {                     /* 解析过程中出过错，别管算出啥都是废的 */
        puts("表达式写错了");
        return;
    }

    if (!isfinite(answer)) {              /* inf / nan 都不是正常的数，比如 1/0 就会得到 inf */
        puts("结果是 inf 或 nan（多半是除 0 了）");
        return;
    }

    /*
     * floor(answer) == answer 说明小数部分是 0，值正好是整数；
     * fabs(answer) < 1e18 是怕那种超大数，%.0f 打出来一大串没意义的位数。
     */
    if (answer == floor(answer) && fabs(answer) < 1e18)
        printf("%.0f\n", answer);   /* 整数值直接按整数打；用 %.0f 而不是转 long long，省得操心溢出 */
    else
        printf("%.10g\n", answer);  /* %g 会自动甩掉多余的 0，最多保留 10 位有效数字 */
}

int main(void)
{
    /* 要跑的测试用例，每条后面注释的是预期结果 */
    const char *testList[] = {
        "(27%4-2)*2+1",                /* 3 */
        "3.5 * 2",                     /* 7，有浮点参与，结果是整数就按整数显示 */
        "2 ^ 3 ^ 2",                   /* 512，右结合，不是 64 */
        "-2 ^ 2",                      /* -4，负号优先级低于幂 */
        "2 ^ -3",                      /* 0.125，负指数 */
        "1 / 3",                       /* 0.3333333333 */
        "10 % 3.5",                    /* 3 */
        "((1.5 + 2.5) * 2) ^ 3 / 4",   /* 128，嵌套括号 + 幂 + 4 */
        "100 % 7.5 * 2",               /* 5，取模和乘混着来 */
        "-3.5 * (2 + 0.5) ^ 2 % 4",    /* -1.875，四则 + 幂 + 取模全上 */
        "1e3 * 1.5",                   /* 1500，指数记法 */
        ".5 + .25",                    /* 0.75，省略整数部分 */
        "0.1 + 0.2",                   /* 0.3，浮点误差被 %.10g 抹平了 */
        "2 * -3 + 8 / 2",              /* -2，乘号后面直接跟负号 */
        "((((7))))",                   /* 7，层层括号 */
        "  3 + 4 * 2  ",               /* 11，前后带空格 */
        "(1+2"                         /* 括号没闭合，应该报错 */
    };

    /* sizeof testList / sizeof *testList 是数组长度，这么写以后加测试用例不用手动改数字 */
    int testCount = (int)(sizeof testList / sizeof *testList);

    for (int i = 0; i < testCount; i++) {
        parseError = 0;                            /* 每条表达式重新开始，别让上一条的错误影响这条 */

        const char *cursor = testList[i];          /* 拿到字符串起始位置 */
        double answer = parseAddSub(&cursor);      /* 从最低优先级那层开始解析，一路算到底 */
        printAnswer(testList[i], answer);          /* 打印原表达式和结果 */
    }

    return 0;
}
