# BigCale

**中文** · [English](#english)

一个用 C 语言写的**大数表达式求值器**。数字大小不受 `int`、`long long`、浮点数精度限制，位数只受机器内存约束；支持小数、科学计数法和整数幂运算。

仓库里有两份等价的源码，逻辑完全相同，只有注释和运行时提示语言不同：

| 文件 | 目标 | 说明 |
| --- | --- | --- |
| `big_cale.c` | `BigCale` | 中文注释 + 中文提示 |
| `big_cale_en.c` | `BigCale_en` | 英文注释 + 英文提示 |

---

## 中文

### 编译

```bash
cmake -S . -B build
cmake --build build
```

也可以直接用编译器：

```bash
gcc -std=c11 -O2 -o big_cale big_cale.c
gcc -std=c11 -O2 -o big_cale_en big_cale_en.c
```

### 用法

```bash
./BigCale              # 先自动跑一遍内置公式，然后问要不要手动输入检测
./BigCale --test       # 跑内置测试（带 PASS/FAIL 判定）
./BigCale --help       # 查看说明
```

手动检测时一行一条表达式，输入 `q`、`quit`、`exit`、`end` 或直接回车结束，`#` 开头的行当作注释跳过。

标准输入不是终端（管道、重定向）时会自动跳过提问，直接逐行读表达式：

```bash
echo "1/3" | ./BigCale
printf "2^100\n(1+2\n" | ./BigCale
```

### 支持的语法

- 运算符：`+` `-` `*` `/` `%` `^` 和括号 `()`
- 数字字面量：`3.14`、`.5`、`1e3`、`2.5E-4`
- 优先级（由低到高）：加减 < 乘除取模 < 一元正负号 < 幂
- 幂是**右结合**：`2^3^2` 按 `2^(3^2)` 算，`-2^2` 按 `-(2^2)` 算
- 不支持函数和变量

### 数值模型

内存里的一个数长这样：

| 字段 | 含义 |
| --- | --- |
| `d` | 数字串，`d[0]` 是最高位，中间不带小数点（堆上分配） |
| `len` | 数字串有效长度 |
| `scale` | 小数点后有几位，真值 = 数字串当整数看 / 10^scale |
| `sign` | `1` 正、`-1` 负 |

例如 `-3.14` 存成 `sign=-1, d="314", len=3, scale=2`。因为内部是"有限小数"表示，精度不会像浮点那样漂移，`0.1 + 0.2` 就是 `0.3`。

### 语义约定

- **除法**保留 `DIV_SCALE`（默认 20）位小数，末位四舍五入，尾部 0 去掉：`1/2` 打印 `0.5`
- **取模**跟 C 的 `fmod` 一致，符号跟着被除数走：`-7 % 3 = -1`。
  算法是"把两边小数点对齐后取整数余数"，所以操作数若来自除法截断，结果会跟 `DIV_SCALE` 有关：
  - `(1/3) % (1/3) = 0`
  - `1 % (1/3) = 0.00000000000000000001`
- **指数必须是整数**：`2^0.5` 是无理数，这种存法存不下，会报"指数必须是整数"。指数大小不限，`2^1000000` 也能算，只是要等
- `0^0 = 1`；`0` 的负次方报错
- **数字大小没有硬上限**。装得下就一直算，只有 `malloc` 失败或位数多到 `int` 都表示不了时才报"内存不够了"——那是物理限制，不是数学限制

### 出错提示

出错时会在表达式下方用 `^` 指出位置：

```
1/0
  ^ 除数不能是 0

(1+2
    ^ 缺少右括号
```

### 代码结构

单文件实现，从上到下大致分层：

1. 小工具：`xmalloc` / `xrealloc` / `skipBlank`
2. 大数本体：`Big` 结构、构造析构、`bigTrim` 归一化
3. 纯数字串运算：`addAbs` / `subAbs` / `mulAbs` / `divmodAbs`（竖式加减乘除）
4. 大数四则运算：`bigAdd` / `bigMul` / `bigDiv` / `bigMod` / `bigPow`（快速幂）
5. 递归下降解析：`parseAddSub` → `parseMulDivMod` → `parseUnarySign` → `parsePower` → `parseOperand`
6. 对外入口 `evalExpression`，以及内置测试表 `kTests`

一层函数管一级优先级，加新运算符不用动主流程。

### 测试

```bash
./BigCale --test
```

内置用例覆盖基本四则、除法精度、取模、幂、大数（如 `2^100`、`2^10000 % 9`）以及各类出错提示和出错位置，输出形如：

```
PASS  0.1 + 0.2
PASS  2 ^ 100
...
54 条测试，54 条通过，0 条失败
```

---

<a name="english"></a>

## English

### Build

```bash
cmake -S . -B build
cmake --build build
```

Or compile directly:

```bash
gcc -std=c11 -O2 -o big_cale big_cale.c
gcc -std=c11 -O2 -o big_cale_en big_cale_en.c
```

### Usage

```bash
./BigCale_en              # run the built-in formulas once, then ask for manual input
./BigCale_en --test       # run the built-in test suite (PASS/FAIL)
./BigCale_en --help       # show help
```

In manual mode, one expression per line. Type `q`, `quit`, `exit`, `end`, or just press Enter to finish; lines starting with `#` are comments and skipped.

When stdin is not a terminal (pipe or redirection) the prompt is skipped and lines are read directly:

```bash
echo "1/3" | ./BigCale_en
printf "2^100\n(1+2\n" | ./BigCale_en
```

### Supported syntax

- Operators: `+` `-` `*` `/` `%` `^` and parentheses `()`
- Numeric literals: `3.14`, `.5`, `1e3`, `2.5E-4`
- Precedence, lowest to highest: add/sub < mul/div/mod < unary +/- < power
- The power operator is **right associative**: `2^3^2` means `2^(3^2)`, `-2^2` means `-(2^2)`
- Functions and variables are not supported

### Number model

A number in memory looks like this:

| Field | Meaning |
| --- | --- |
| `d` | digit string, `d[0]` is the most significant digit, no decimal point stored (heap allocated) |
| `len` | number of significant digits |
| `scale` | digits after the decimal point; the real value is `digits / 10^scale` |
| `sign` | `1` positive, `-1` negative |

For example `-3.14` is stored as `sign=-1, d="314", len=3, scale=2`. Because values are kept as exact finite decimals, precision never drifts the way floating point does: `0.1 + 0.2` is exactly `0.3`.

### Semantics

- **Division** keeps `DIV_SCALE` (20 by default) decimals, rounds the last digit and strips trailing zeros: `1/2` prints `0.5`
- **Modulo** matches C's `fmod`: the sign follows the dividend, `-7 % 3 = -1`. The algorithm aligns both decimal points and takes the integer remainder, so operands produced by a truncated division depend on `DIV_SCALE`:
  - `(1/3) % (1/3) = 0`
  - `1 % (1/3) = 0.00000000000000000001`
- **The exponent must be an integer**: `2^0.5` is irrational and cannot be represented here, so it reports "exponent must be an integer". The magnitude of the exponent is unbounded: `2^1000000` works, it just takes a while
- `0^0 = 1`; a negative power of `0` is an error
- **There is no hard cap on number size.** It keeps going as long as things fit, and reports "out of memory" only when they really do not (`malloc` failed, or the digit count exceeds what an `int` can express) - a physical limit, not a mathematical one

### Diagnostics

On error, a caret points at the offending position:

```
1/0
  ^ division by zero

(1+2
    ^ missing right parenthesis
```

### Code layout

Single-file implementation, layered top to bottom:

1. Utilities: `xmalloc` / `xrealloc` / `skipBlank`
2. Big number core: the `Big` struct, construction/destruction, `bigTrim` normalization
3. Pure digit-string arithmetic: `addAbs` / `subAbs` / `mulAbs` / `divmodAbs` (grade-school add/sub/mul/div)
4. Big number arithmetic: `bigAdd` / `bigMul` / `bigDiv` / `bigMod` / `bigPow` (binary exponentiation)
5. Recursive descent parsing: `parseAddSub` → `parseMulDivMod` → `parseUnarySign` → `parsePower` → `parseOperand`
6. The public entry point `evalExpression`, plus the built-in `kTests` table

One function per precedence level, so adding an operator never touches the main flow.

### Tests

```bash
./BigCale_en --test
```

The built-in cases cover basic arithmetic, division precision, modulo, powers, big numbers (`2^100`, `2^10000 % 9`), and every diagnostic message with its caret position. Output looks like:

```
PASS  0.1 + 0.2
PASS  2 ^ 100
...
54 tests, 54 passed, 0 failed
```
