#include <stdio.h>
#include <ctype.h>
#include <math.h> // 引入pow函数

// 函数声明
int evaluate(char **s);

// 解析数字，包括括号内的表达式
int parseNumber(char **s) {
    if (**s == '(') {
        (*s)++; // 跳过左括号
        int num = evaluate(s); // 计算括号内的表达式
        (*s)++; // 跳过右括号
        return num;
    } else {
        int num = 0;
        while (isdigit(**s)) {
            num = 10 * num + (**s - '0');
            (*s)++;
        }
        return num;
    }
}

// 处理次幂运算
int evaluatePower(char **s) {
    int base = parseNumber(s);
    while (**s == '^') {
        (*s)++; // 跳过'^'
        int exponent = parseNumber(s); // 解析指数
        base = (int) pow((double) base, (double) exponent);
    }
    return base;
}

// 处理乘除求模运算
int evaluateMD(char **s) {
    int num = evaluatePower(s); // 先处理可能的次幂运算
    while (**s == '*' || **s == '/' || **s == '%') {
        char op = *(*s)++;
        int nextNum = evaluatePower(s); // 对于乘除，也要先处理次幂
        if (op == '*') {
            num *= nextNum;
        } else if (op == '/') {
            num /= nextNum;
        } else {
            num %= nextNum;
        }
    }
    return num;
}

// 评估整个表达式
int evaluate(char **s) {
    int result = evaluateMD(s);
    while (**s && **s != ')' && **s != '%') { // 修改条件，确保在遇到右括号或求模运算符时停止
        char op = *(*s)++;
        if (op == '+' || op == '-') {
            int nextNum = evaluateMD(s);
            if (op == '+') {
                result += nextNum;
            } else {
                result -= nextNum;
            }
        }
    }
    return result;
}

// 主函数
int main() {
    char expression[] = "(27%4-2)*2+1";
    char *p = expression;
    int result = evaluate(&p);
    printf("结果是: %d\n", result);
    return 0;
}
