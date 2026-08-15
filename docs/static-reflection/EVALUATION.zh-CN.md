# 反射 vs 宏 vs 概念：实测评估笔记

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验性工作）
> **工具链**：GCC 16.1.0（`g++-16`），标志 `-std=c++26 -freflection`（反射）、
> `-std=c++20`（概念）、`-std=c++11`（基线 enable_if）
> **状态**：2026-08-15 在本文档对应的机器上重新实测（12 代 i5-12600KF，无 ccache，
> g++-16 16.1.0-2ubuntu1）；原始运行记录见测量日志（§5）。下面的每一个数字都
> 是在本次会话中重新推导的——本文件的先前版本带着几个**经不起**重新测量的数字；
> §5 精确列出了哪些论断被修正。

本文档记录*我们如何*评估了某个仅有头文件的模板库（nlohmann/json）的两个现代化
方向——(A) 用 C++20 概念替换手写宏/SFINAE；(B) 用 P2996 静态反射替换面向用户的
宏。目标是分享**评估操作**（测量设置、对比轴、陷阱），让其他人能够复现或质疑这些
数字，而不只是接受结论。

---

## 0. 三个评估问题

对每个方向，我们都提出同样四个问题，并用可测量的证据而非观点来回答：

| # | 问题 | 指标 |
|---|---|---|
| Q1 | 能否减少代码？ | 每种类型的用户侧行数、边际成本、盈亏平衡点 |
| Q2 | 编译期成本？ | 在**同一标准**下对**同一编译单元**做墙钟时间 / 峰值 RSS，改造前 vs 改造后 |
| Q3 | 二进制 / 生成代码是否膨胀？ | 可执行文件大小、`size` 的 text/data、段归属 |
| Q4 | 诊断更好还是更差？ | 错误行数、首条错误、失败是否静默 |

再加上一个最终被证明最重要的正确性问题：

| Q5 | 非开心路径是否工作？ | 私有成员、嵌套类型、ADL 定制 |

---

## 1. 评估操作 A：概念 vs enable_if（双路径）

该库对同一组重载同时保留 C++11 enable_if 路径与 C++20 概念路径
（`#ifdef JSON_HAS_CPP_20`）。双路径的要点就在于**两者必须选中同一组重载**。
因此评估通过在不同标准下编译同一编译单元来对比。

### 1.1 设置 —— 基线很重要（前后对比，标准与标志都要一致）

现代化目标是 **C++26 + 静态反射**（分支目标），所以头条对比必须是前（develop
基线 `cdf52ae9`，纯 enable_if）vs 后（本分支）在 `-std=c++26 -freflection` 下
进行。双路径位于**同一批头文件**（`#ifdef JSON_HAS_CPP_20`），所以完整矩阵还要
分别隔离出 概念切片（c++20）、标准升级（c++20→c++26）、以及 `-freflection`
标志成本——这些都不应该混为一谈。

测量协议：每个配置编译 **3 次**，报告最小的墙钟时间（模板繁重的编译噪声很大；
单次运行不算数）。大小采用十进制 KB（1 KB = 1000 B），与 `ls`/`stat` 一致。

```sh
# 基线：git worktree add /tmp/json-baseline develop   (cdf52ae9)
FLAGS="-Wno-deprecated -Wno-float-equal -Wno-deprecated-declarations
       -DDOCTEST_CONFIG_SUPER_FAST_ASSERTS -DJSON_TEST_KEEP_MACROS
       -DJSON_TEST_USING_MULTIPLE_HEADERS=1 -Itests/thirdparty/doctest
       -Itests/thirdparty/fifo_map"

# 头条：完整现代化，c++26 + freflection 下 前 vs 后
/usr/bin/time -f "wall=%e s maxrss=%M KB" \
  g++-16 -O0 -std=c++26 -freflection $FLAGS -I/tmp/json-baseline/include \
  -c tests/src/unit-serialization.cpp -o /tmp/before.o
/usr/bin/time -f "wall=%e s maxrss=%M KB" \
  g++-16 -O0 -std=c++26 -freflection $FLAGS -Iinclude \
  -c tests/src/unit-serialization.cpp -o /tmp/after.o
```

**陷阱（我们犯过的错）**：对比 c++11 vs c++20 会把 *标准升级* 的成本当成概念的
成本；而把所有东西都钉在 c++20 又会掩盖 C++26 + 反射这个目标。公平的对比要把
标准与标志都固定。

### 1.2 结果（unit-serialization.cpp，单个编译单元）

**头条：完整现代化（前 vs 后 @ c++26 -freflection，取 3 次最小值）：**

| | 前（develop） | 后（分支） | 差值 |
|---|---|---|---|
| -O0 墙钟 | 3.55 s | 3.59 s | **+1.1%（噪声）** |
| -O0 .o | 2,612 KB | 2,628 KB | +0.6% |
| -O2 墙钟 | 6.36 s | 6.35 s | **−0.2%（噪声）** |
| -O2 .o | 700 KB | 700 KB | ~0% |

**完整现代化（概念 + 反射就绪）在目标标准下成本 ~0**：墙钟 −0.2%~+1.1%、代码
大小 ≤0.6%。两者都实例化同一个 `external_constructor<...>::construct`；概念只做
重载选择，而且反射头文件根本没被该编译单元包含。

**成本分解（逐个隔离因子，-O0，取 3 次最小值）：**

| 配置 | 墙钟 | 隔离出的因子 |
|---|---|---|
| 前 @ c++20 | 3.14 s | 基线 |
| 前 @ c++26 | 3.44 s | **+9.6%** 标准升级 |
| 前 @ c++26 -freflection | 3.55 s | **+3.2%** 反射标志 |
| 后 @ c++20 | 3.15 s | 概念切片 **≈ 0%** |
| 后 @ c++26 -freflection | 3.59 s | 完整现代化 |

**分解的要点**：
- c++20 下的概念切片：**≈ 0%** —— 概念双路径是免费的（旧报告 −5% 未复现，见 §5）
- 标准升级 c++20→c++26：**+9.6%**（旧报告 +3%）
- `-freflection` 标志（即使编译单元里没有反射代码）：**+3.2%**
  （旧报告 +7–8%）
- 目标标准下的完整现代化：**≈ 0–1%**（各因子大致相互抵消）

跨标准参考（全部取最小墙钟；-O0 的 .o 大小见括号）：

| 路径 | -O0 墙钟 | -O2 墙钟 | -O0 的 .o |
|---|---|---|---|
| c++11（enable_if） | 2.28 s | 4.89 s | 2,555 KB |
| c++20（概念）  | 3.15 s | 5.44 s | 2,595 KB |
| c++26 +refl（分支） | 3.59 s | 6.35 s | 2,628 KB |

运行时（全新基准编译单元：对含 200 个对象的复杂 JSON 迭代 200 次，-O2，
c++26 下 前 vs 后，各自交错运行 6 次；源码见 §5）：

| | 前 | 后 | 结论 |
|---|---|---|---|
| 序列化 | 0.064–0.068 ms/op | 0.069–0.074 ms/op | 在构建布局噪声范围内（±5–8%：重新构建*同一份*源码就能造成这么大的波动） |
| 解析 | 0.411–0.433 ms/op | 0.406–0.422 ms/op | 无回退（区间重叠） |

前/后的 `dump()` 代码是逐字节一致的（分支改动的是词法分析器和概念双路径，不是
序列化器），所以序列化的"差距"是代码布局假象：对*相同*源码的三次构建在
13.04–14.14 ms/200 次循环范围内波动，和前后差异量级相同。解析最多无变化，最好
还略有加速（词法分析器 `from_chars`）。

### 1.3 诊断评估（Q4）

有趣的情形是*失败*：一个没有 `to_json` 的类型。目标标准（c++26 -freflection），
前 vs 后：

```sh
cat > err.cpp <<'EOF'
#include <nlohmann/json.hpp>
struct not_serializable { int x; };
int main() { nlohmann::json j; not_serializable ns; nlohmann::to_json(j, ns); }
EOF
g++-16 -std=c++26 -freflection -I/tmp/json-baseline/include -fsyntax-only err.cpp 2> before.txt
g++-16 -std=c++26 -freflection -Iinclude -fsyntax-only err.cpp 2> after.txt
wc -l before.txt after.txt   # 238 vs 376  （精确复现）
```

| | 前（enable_if） | 后（概念） |
|---|---|---|
| 错误输出 | 238 行 / 27,987 B | 376 行 / 42,478 B（行数 +58%，字节 +52%） |
| 首条错误 | `err.cpp:3:70: error: no match for call to '(const ...::to_json_fn)(json&, not_serializable&)'` | 首条错误相同 |
| 为什么 | enable_if 失败不透明（`enable_if<false>` 从不说明*为什么*） | 7 条 `required for the satisfaction of ...` 链，结尾是 `the required expression 'std::begin(t)' is invalid` |

**解读**：概念输出约 1.6 倍长，但每次失败都会点名*原因*（哪个探针失败、失败在
哪个概念行）。两条路径共享同一条首错行（调用点 + `no match for call to
'to_json'`），所以"哪里出错了"的锚点是相同的；概念只是补充了"为什么"。这些计数
**精确复现了上一版报告**（238 vs 376）。

**陷阱**：通过 `j = ns`（赋值）来对比错误是无用的——它在 `basic_json` 自己的
构造函数 SFINAE 处就失败了，根本到不了 `to_json`，且在两个标准下表现相同。你必须
直接调用 `nlohmann::to_json(j, ns)` 才能触发你所改动的那组重载。

### 1.4 非开心路径：alt_string 回归（Q5）

概念层在 `string_like`/`object_like`/`array_like` 里携带 `BasicJsonType`。用默认的
`std::string` string_t 做评估时，即使概念声明为错误的参数顺序也能通过，因为错误的
绑定恰好"对上号"了。需要一个**自定义 string_t 编译单元**（`tests/src/unit-alt-string.cpp`）
才暴露出真正的 bug：

- 概念声明为 `template<typename B, typename T>`（BasicJsonType 在前）
- 约束占位符 `concepts::string_like<BasicJsonType> S` —— GCC 把 `S` 绑定到
  **第一个**参数、把 `<BasicJsonType>` 绑定到 **第二个**
- ⇒ 对自定义的 `alt_string` string_t，用户类型落进了 `B`，于是 `typename B::string_t`
  变成硬错误（`no type named 'string_t' in 'class alt_string'`）
- 修复：声明为 `template<typename T, typename B>`（候选在前）；用最小复现验证
  （见下）

**评估教训**：只对默认类型做差分探针**不够**；矩阵里至少需要一个非默认
模板参数的编译单元。最小复现（在 g++-16、`-std=c++20` 下重新验证——正确顺序可编译，
错误顺序以 `no type named 'string_t' in 'struct alt_string'` 失败）：

```cpp
template<typename T, typename B>
concept StringLikeRight = std::is_constructible_v<typename B::string_t, T>;
template<typename B, StringLikeRight<B> S> void f(const S&);   // <T, B>: 通过
// 若声明成 <B, T> 则反而失败 -> B 会绑定到 S!!  （在 alt_string 上硬报错）
```

另见 AGENTS.md §3 "Partial-application concepts carry BasicJsonType LAST"。

---

## 2. 评估操作 B：反射 vs 宏

问题："P2996 反射能否让我们删掉 `NLOHMANN_DEFINE_TYPE_INTRUSIVE`？"

### 2.1 设置

基准探针 `tests/static-reflection/bench_macro_vs_reflection.cpp`：
N 个 person 类似结构体 `{ name (std::string), age (int), height (double),
tags (std::vector<std::string>), active (bool) }`，由 X-macro 生成。同一个编译单元
通过 `-DBENCH_*` 标志切换**三种模式**：

- **宏版本**（仅 `-DBENCH_N`）：`NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_i, name, ...)`
  —— 每个结构体内一条宏调用
- **refl v1**（`-DBENCH_REFLECTION`）：朴素的通用 `to_json`/`from_json`，基于
  `nonstatic_data_members_of`；用户类型**零**声明
- **refl2 v2**（`-DBENCH_ADL_REFLECTION`）：ADL 感知的递归序列化器（本修订新增）——
  显式调用 `adl_serializer`、容器逐元素递归、逐成员反射递归、默认 `unprivileged()`

`-DBENCH_N`（默认 50）控制 100 个已定义类型中有多少被 `main()` 实例化（未使用的
类型不生成代码）。三种模式都用相同的标志编译（`-std=c++26 -freflection`），且
`main()` 保持序列化器可观测（其输出喂给一个打印用的计数器），因此 `-O2` 无法消除
生成的代码——这是刻意的设计选择，见下面的注意事项。三种模式打印相同的计数器
（JSON 输出一致），所以 §2.2 的数字纯粹是序列化器的成本。

反射序列化器 **v1**（一次性成本；探针里的 `namespace refl` 块是 **24 行**，
加 1 行 `#include <meta>` = 25 行）：

```cpp
#include <meta>
namespace refl {
template<typename BasicJsonType, typename T>
void to_json(BasicJsonType& j, const T& v) {
    j = BasicJsonType::object();
    template for (constexpr auto m : std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))) {
        j[std::string(std::meta::identifier_of(m))] = v.[:m:];
    }
}
template<typename BasicJsonType, typename T>
void from_json(const BasicJsonType& j, T& v) {
    template for (constexpr auto m : std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))) {
        using M = typename [: std::meta::type_of(m) :];
        v.[:m:] = j.at(std::string(std::meta::identifier_of(m))).template get<M>();
    }
}
}
```

**构建 v1 时踩到的陷阱（全部都是已验证的 GCC 16 行为；与上一版一致）**：
- `std::meta::` 必须完整限定；`std::define_static_array`（不是
  `std::meta::define_static_array`）
- `nonstatic_data_members_of` 返回一个**临时 vector**：直接在调用上取下标/取大小，
  千万不要绑定到局部 `constexpr`
- `template for` 的范围要么内联，要么是命名空间作用域的 constexpr
- 嵌套类成员的递归序列化**不会**落入 nlohmann 的 ADL 重载集（`detail::to_json` 不是
  裸函数）；标量必须走赋值 / `.template get<M>()` —— **v1 完全无法序列化嵌套的
  普通结构体或普通结构体容器**
- `if constexpr` 不会丢弃 false 分支 ⇒ 类-对-标量递归需要 tag 分发

反射序列化器 **v2（"refl2"）** —— 本修订新增的 ADL 感知递归编解码器（模式
`-DBENCH_ADL_REFLECTION`；参考实现由 `tests/static-reflection/probe_adl_recursion.cpp`
验证，见 §2.5）。它在序列化器内部自己实现递归分发：

```
serialize_one(j, v)，按优先级从高到低：
  5  嵌套 basic_json 值                -> j = v               （原生嵌套）
  4  adl 分支（字符串 + 非容器的 adl 可用类型） -> adl_serializer<T, B>
  3  类数组容器                        -> 逐元素递归
  2  类对象容器（map 类）              -> 逐值递归（字符串键）
  1  可反射结构体                      -> 逐成员反射递归
  0  其他任何类型                      -> static_assert 诊断
```

- adl 分支显式调用 `nlohmann::adl_serializer<T, B>` —— 公开扩展点 —— 因此
  **定制优先于反射**：带用户 `to_json` 的嵌套成员先被扩展机制捕获（这正是 v1
  递归陷阱的修复），而嵌套的普通结构体落到反射分支继续递归。
- 检测 trait（`is_adl_serializable`、`is_reflectable_struct` 等）只建立在公开 API
  之上 —— 完全不依赖库的 `detail` 内部。分发用 `priority_tag` 重载排序（与
  nlohmann 内部使用的同一惯用法）；任何地方都没有 `if constexpr` 加 splice。
- 访问策略：反射默认用 `access_context::unprivileged()`（`codec<false>`）——
  私有成员既不序列化也不解析；`codec<true>` 切换到 `unchecked()` 供库内部使用
  （§2.3 的两层策略，§2.5 中有演示）。
- 整个 `namespace refl2` 块是 **311 行**，加上（共用的）`#include <meta>`
  = 312 行一次性成本。

构建 v2 时新踩到并修复了两个陷阱（探针里都有复现）：
- **嵌套 json 的 string_like 陷阱**：`basic_json` 有一个显式模板转换运算符
  （委托给 `get<ValueType>()`），所以 `std::is_constructible_v<std::string, json>`
  为 TRUE。nlohmann 自己的 `string_like` 概念因此会匹配一个 `json` **值**，
  `adl_serializer<json, json>::to_json` 解析到字符串路径，对非字符串值调用
  `get<std::string>()` 并抛出异常。修复：嵌套 json 分支（优先级 5）排在 adl
  分支之前。
- **from_json 对容器的过度接受**：nlohmann 的 from_json 检测把
  `map<string, PlainStruct>` 视为 adl 可用（tuple 机制让
  `get<pair<const string, PlainStruct>>` 看起来可行），但实例化时会崩。修复：
  类数组/类对象容器被排除在 adl 分支之外（`adl_branch_eligible`），一律走
  编解码器自己的逐元素递归 —— 对 nlohmann 能处理的类型行为完全一致，而且额外
  覆盖了普通反射结构体的容器。

### 2.2 结果

同一会话内重新测量（三种模式、同一编译单元、同一 `-std=c++26 -freflection`，
取 3 次最小值；原始数据见 §5）。三种模式打印相同的计数器——JSON 输出完全一致——
所以这些数字纯粹是序列化器的成本，而非行为漂移。

**Q1 节省的代码**（每种类型的用户行数：宏 = 结构体 + 宏调用 = 2/类型；
反射 = 仅结构体 = 1/类型；一次性成本：v1 = 25 行，v2 = 312 行）：

| 类型数 | 宏 | refl v1 | refl2 v2 |
|---|---|---|---|
| 1 | 2 | 26 | 313 |
| 20 | 40 | 45 | 332 |
| **25** | 50 | 50 | 337 |
| 50 | 100 | 75 | 362 |
| 100 | 200 | 125 | 412 |
| **312** | 624 | 337 | 624 |

两个反射版本每类型都**省 1 行**；朴素的 v1 需要**25 行**一次性序列化器
（盈亏平衡 ≈ 25 个类型），但只适用于扁平结构体；而完整的 v2（递归 + ADL +
私有成员策略）需要**312 行**一次性序列化器（盈亏平衡 ≈ 312 个类型），能处理
嵌套结构体、普通结构体容器、用户定制与 `unprivileged()` 访问控制——这些 v1
完全做不到。

**Q2 编译时间**（同一编译单元、同一标志，取 3 次最小值）：

| N | 宏 -O0 | v1 -O0 | v2 -O0 | 宏 -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 2.23 s | 2.34 s | 2.27 s | 3.12 s | 3.31 s | 3.12 s |
| 20 | 2.31 s | 2.52 s | 2.45 s | 3.23 s | 3.81 s | 3.31 s |
| 28 | 2.40 s | 2.63 s | 2.52 s | 3.41 s | 4.11 s | 3.45 s |
| 50 | 2.63 s | 2.87 s | 2.78 s | 3.88 s | 4.99 s | 4.00 s |
| 100 | 3.06 s | 3.51 s | 3.35 s | 4.99 s | 6.46 s | 4.92 s |

v2 在每个测量点上**都比 v1 编译更快**（-O0 下 −2.8…−4.6%，-O2 下 −6…−24%），
且接近宏基线（-O0 下 +1.8…+9.5%；-O2 下 −1.4…+3.1%）。分发机制在编译期解析，
逐类型的实例化工作量比 v1 的逐成员 `j[...] = v.[:m:]` 加
`std::string(identifier_of(m))` 代码生成更少。

**Q3 二进制**（各 N 下可执行文件字节 / `size` text）：

| N | 宏 -O0 | v1 -O0 | v2 -O0 | 宏 -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 393,656 / 169,117 | 484,512 / 198,990 | 395,632 / 169,901 | 113,384 / 80,047 | 119,888 / 85,148 | 113,384 / 80,047 |
| 20 | 445,936 / 202,452 | 555,608 / 260,004 | 501,944 / 217,336 | 131,088 / 94,096 | 174,456 / 129,198 | 131,088 / 94,128 |
| 28 | 470,560 / 216,484 | 584,264 / 285,692 | 546,544 / 237,304 | 145,120 / 106,145 | 194,248 / 149,898 | 145,120 / 106,177 |
| 50 | 530,088 / 255,090 | 669,208 / 356,342 | 666,120 / 292,223 | 182,608 / 138,230 | 265,248 / 207,818 | 182,608 / 138,262 |
| 100 | 667,600 / 342,916 | 850,360 / 517,028 | 939,752 / 417,157 | 266,640 / 208,829 | 421,720 / 342,878 | 266,640 / 208,861 |

**头条发现**：在 **-O2 下，v2 与宏版本逐字节相同**——每个 N 的可执行文件都
完全相等（`text` 相差不超过 32 字节；N=50 时 `nm` 符号数与宏一致，都是 288，
而 v1 是 300）。v2 的分发（priority_tag 排序、adl_serializer 间接、反射递归）
在优化下完全塌缩：两条路径最终收敛到宏直接生成的同一套
`adl_serializer`/`external_constructor` 代码。v1 不会收敛（其逐成员的字符串键
转换加赋值在优化后存活：-O2 下 exe +6…+58%）——所以 v2 在 -O2 下比 v1
**小 −5…−37%**。

在 **-O0** 下，v2 介于宏与 v1 之间：N=1 时接近宏（+0.5%），到 N=100 时增长到
+40.8% exe（v1 为 +27.4%）。-O0 的开销是未塌缩的分发：N=50 时 `nm` 计数 v2 为
1,871 个符号，v1 1,621、宏 1,471。旧的 §2.2 教训依然成立——报告编译期/二进制
数字必须同时说明优化级别并保持生成的代码可观测——但新发现是：v2 的 -O0 膨胀
*大多是优化残留*：到 -O2 就蒸发为恰好为零。

**Q4 诊断** —— 用刻意构造的坏类型（`std::mutex` 成员，既不可 adl 也不可反射）验证：

| 场景 | 宏 | refl v1 / refl2 v2 |
|---|---|---|
| 成员在宏里**列出**但不可序列化 | 编译错误，在宏展开处点名 `std::mutex` | 编译错误，在序列化器那一行点名 `std::mutex` |
| 成员在宏里**没列出** | **静默**有损往返（成员被丢弃，零诊断） | 不可能：反射看到所有公有成员——公有不可序列化成员就是编译错误 |
| 仅 v2：不可序列化的*类型*（无 to_json、不可反射） | — | v2 只发**一条**带可操作信息的 static_assert（"定义 to_json 或特化 nlohmann::adl_serializer"）：11 行错误 vs 同一失败走库自身重载集时的 238 行（enable_if）/ 376 行（concepts） |

解读（与上一版一致）：对*已列出*的不可序列化成员，宏和反射都在编译期失败；
宏真正的隐患是静默*遗漏*，反射在结构上不可能遗漏。v2 额外把整个"类型未处理"
的情况变成一条可操作的 static_assert，因为分发就在序列化器内部，不依赖库的
（冗长的）重载集错误路径。

### 2.3 修正：私有成员是可反射的

项目早先的 M2 结论声称 `basic_json::json_value`（私有嵌套 union）从外部不可达，
被迫采用"镜像 union"。那是**错的**，而抓住它的评估操作值得分享：

```cpp
// consteval 上下文 —— 可行
consteval auto json_value_info() {
    constexpr auto m_data = std::meta::nonstatic_data_members_of(
        ^^json, std::meta::access_context::unchecked())[0];      // data
    constexpr auto m_value = std::meta::nonstatic_data_members_of(
        std::meta::type_of(m_data), std::meta::access_context::unchecked())[1]; // m_value
    return std::meta::type_of(m_value);                            // json_value
}
using real_json_value = typename [: json_value_info() :];
```

由 `probe_real_json_value.cpp` 重新验证（通过；打印全部 8 个成员）：
`object, array, string, binary, boolean, number_integer, number_unsigned,
number_float`。

| 操作 | 结果 |
|---|---|
| 直接 `[: ^^ json::json_value :]` | ✗ `is private within this context` |
| `unchecked()` 枚举私有数据成员 | ✓ |
| `type_of` → 私有嵌套类型 | ✓ |
| 在 **consteval 函数**里枚举其成员 | ✓（8 个成员） |
| 在 **`template for` 内联**里枚举 | ✗ `not a complete class type`（GCC 16） |
| splice 构造 / 读写 / 分配-释放 | ✓（ASan 干净） |

**这是最有价值的单一评估发现**："不能反射私有嵌套类型"是 `template for` 内联上下文
的假象，不是反射的局限。同样的代码放进 consteval 函数就能工作。探针：
`tests/static-reflection/probe_real_json_value.cpp`。

对实现的推论：`reflection_json.hpp` 里的 schema 表（`kStorage`、`kMemberIds`）
现在**从真实的 `basic_json::json_value`** 生成，并用一个 `static_assert` 把本地存储
union 与真实成员表绑定——镜像漂移现在是编译期错误，而不是静默失配。本地普通 union
仅作为平凡的存储载体保留（无构造函数 ⇒ 无堆分配契约），这是刻意的、已记录的选择。

### 2.4 "反射真实私有 union"改造的成本（实测）

把 schema 的来源从镜像 union 换成真实的 `json_value`（consteval 间接路径 +
static_assert）之后，我们重新测量了 `m2_diff.cpp`——分别对旧头文件
（`git show 7f37cc7f^:include/nlohmann/reflection_json.hpp`）与当前头文件编译，
同一编译单元、`-std=c++26 -freflection`、g++-16、取 3 次最小值：

| | 旧（镜像） | 新（真实 + static_assert） |
|---|---|---|
| -O0 编译墙钟 | 1.93 s | 1.93 s（在噪声内） |
| -O2 编译墙钟 | 2.06 s | 2.10 s（在噪声内） |
| -O0 峰值 RSS | ~354 MB | ~354 MB（+0.1%） |
| -O0 可执行文件 | 172,640 B | 172,640 B（**逐字节相同**） |
| -O2 可执行文件 | 27,600 B | 27,600 B（**逐字节相同**） |
| -O0 / -O2 的 text | 64,673 B / 16,175 B | 相同 |
| 行为 | M2 DIFF TEST PASSED | PASSED；ASan 干净（退出码 0） |

**要点**：反射真实私有嵌套 union 在编译期成本几乎为零、运行时成本为零——schema 是
`constexpr`，所以两个来源产出相同的表，而且 `static_assert` 会在求值后消失。这个
设计唯一的真实成本是 consteval 管道（仅 consteval 可枚举的陷阱），不是时间或大小。
这些数字**精确复现了上一版报告**（两个优化级别下可执行文件逐字节相同）。这加强了
"凡是类型可达就优先用真实类型、而不是镜像"的论据。

---

### 2.5 ADL 感知递归编解码器：探针结果

`tests/static-reflection/probe_adl_recursion.cpp` 验证 v2 设计所依赖的三条路径
（18 项检查全部 PASS，-O1 下 ASan 干净）：

- **[A] 嵌套普通结构体** —— 结构体套结构体、`vector<PlainStruct>` 与
  `map<string, PlainStruct>` 成员通过反射/容器分支序列化并往返（精确 JSON 与
  重序列化双重检查）。
- **[B] ADL 定制优先级** —— 一个可反射的结构体（`mine::Date`，3 个公有成员）只要
  有用户 `to_json`，就序列化成用户自定义的字符串形式而非反射出的对象；显式
  `nlohmann::adl_serializer` 特化同样有效；嵌套在反射 `Event` 结构体里的自定义
  类型（一个 `Date` 成员和一个 `vector<Date>` 成员）被 adl 分支捕获——§2.1 的
  递归陷阱已修复——且与同一值的原生 `nlohmann::to_json` 输出逐字节相等。
- **[C] 私有成员** —— `codec<false>`（unprivileged）只序列化一个含私有字段的
  类的 2 个公有成员，且 `from_json` 不触碰私有成员；`codec<true>`（unchecked）
  序列化全部 3 个。分类事实也用 static_assert 固化：只有私有成员的类仅在
  unchecked() 下可反射，成员计数为 2 vs 3。
- **[E] v1 对齐** —— 在扁平结构体上，v2 输出与 v1 朴素序列化器完全一致
  （v1 已能处理的情形没有回归）。

外加两个塑造了设计走向的陷阱（见 §2.1）：嵌套 json 的 string_like 陷阱与
from_json 对容器的过度接受，都被优先级排序复现并绕开。

两个值得记录的负面事实：一个 `json` **值**对 nlohmann 自己的 trait 是 adl 可用
的（经由 string_like 陷阱），但必须原生拷贝——嵌套 json 分支排在 adl 分支之前；
v2 的分发在本地完成，因此不支持的类型的错误是一条可操作的 static_assert，
而不是库的 238–376 行错误级联。

## 3. 评估方法论要点

1. **前后对比必须在目标标准下把标准与标志都固定** —— 对比 c++11 vs c++20 会把
   标准升级成本（这里是 +9.6%，不是 +3%）误归给概念的改动；把一切都钉在 c++20 又
   会掩盖 C++26 + `-freflection` 目标（标志成本 +3.2%，不是 +7–8%）。C++26 现代化
   的头条对比是前（develop）vs 后（分支）在 `-std=c++26 -freflection` 下进行；完整
   矩阵再逐个隔离各因子。真实总成本：≈0–1%。
2. **给每个编译期/二进制数字都标注优化级别，并保持生成代码可观测** —— 一旦基准
   阻止了死代码消除，反射-对-宏的差距在 -O2 下就不再坍缩：-O2 下 exe +24–58%
   （随 N 增大）vs -O0 下 +23–27%。一个 -O2 构建会消除被测代码的基准，什么都测不到。
3. **对比失败模式而不只是成功——但要去核实"静默"到底指什么** —— 对已列出的不可
   序列化成员，宏与反射两条路径都在编译期失败；宏真正的隐患是*遗漏*（一个没写进
   `NLOHMANN_DEFINE_TYPE_INTRUSIVE` 的成员会被静默地从序列化里丢弃，零诊断），而反射
   在结构上做不到遗漏。
4. **探针矩阵里非默认模板参数是必须的** —— 概念的参数顺序 bug 在 `std::string` 上
   看不见。
5. **consteval 与 `template for` 上下文会改变 GCC 允许你反射的内容** —— 如果某个
   反射查询在一个上下文失败，先在 consteval 辅助函数里再试一次，再下"不可能"的结论。
6. **`-freflection` 不能放进 `CMAKE_CXX_FLAGS`** —— 它会泄漏进以默认标准编译的嵌套
   子项目 `TryCompile` 探针并失败。（预设修复 + AGENTS.md 规则。）

## 4. 复现

```sh
# 概念 vs enable_if（编译时间 / .o 大小；取 3 次运行的最小值才是数字）
FLAGS="-Wno-deprecated -Wno-float-equal -Wno-deprecated-declarations
       -DDOCTEST_CONFIG_SUPER_FAST_ASSERTS -DJSON_TEST_KEEP_MACROS
       -DJSON_TEST_USING_MULTIPLE_HEADERS=1 -Itests/thirdparty/doctest
       -Itests/thirdparty/fifo_map"
git worktree add /tmp/json-baseline develop          # cdf52ae9
/usr/bin/time -f "wall=%e s" g++-16 -O0 -std=c++26 -freflection $FLAGS \
  -I/tmp/json-baseline/include -c tests/src/unit-serialization.cpp -o /tmp/b.o
/usr/bin/time -f "wall=%e s" g++-16 -O0 -std=c++26 -freflection $FLAGS \
  -Iinclude -c tests/src/unit-serialization.cpp -o /tmp/a.o

# 反射 vs 宏 —— 三种模式（已入库的基准编译单元，见 §2.2）
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
  -o /tmp/bm tests/static-reflection/bench_macro_vs_reflection.cpp      # macro
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_REFLECTION -Iinclude \
  -o /tmp/br tests/static-reflection/bench_macro_vs_reflection.cpp      # refl v1
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_ADL_REFLECTION -Iinclude \
  -o /tmp/br2 tests/static-reflection/bench_macro_vs_reflection.cpp     # refl2 v2
size /tmp/bm /tmp/br /tmp/br2 && nm /tmp/bm /tmp/br /tmp/br2 | wc -l

# ADL 感知递归编解码器探针（§2.5）：嵌套 / ADL / 私有 / 对齐
g++-16 -std=c++26 -freflection -O0 -Iinclude \
  -o /tmp/par tests/static-reflection/probe_adl_recursion.cpp && /tmp/par

# 真实私有 json_value 探针 + 差分（行为一致 + ASan）
g++-16 -std=c++26 -freflection -O0 -Iinclude \
  -o /tmp/prjv tests/static-reflection/probe_real_json_value.cpp && /tmp/prjv
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
```

所有探针都在 `tests/static-reflection/`；每个文件的头部都记录了自己的构建方式。
上面的数字是在分支所在机器上采集的（GCC 16.1.0、x86-64、无 ccache）；绝对数值会随
硬件变化，**比值和方法论才是重点**。

## 5. 测量日志与相对上一版的修正

2026-08-15 在这台机器上重新测量（12 代 i5-12600KF，10 核，g++-16 16.1.0-2ubuntu1，
无 ccache；基线 develop `cdf52ae9`，分支 `cca6c3c5`）。原始运行记录（每个配置 3 次、
取最小值）存放在本次会话的 `/tmp/eval-20260815/results/{matrix1,matrix2,matrix3,runtime,probes}.txt`
（未入库的工作产物）。

三模式重测（本会话、同一台机器、同一工具链；宏/v1 与新的 v2 模式在同一次扫描中
重跑，取 3 次最小值；原始数据在 `build/scratch/measure_results.txt`，未入库）：
宏与 v1 的可执行文件大小在每个 N 上都与上一版报告完全一致（如 N=50 -O0：
530,088 / 669,208；N=100 -O2：266,640 / 421,720）；新的 v2 模式在 **-O2 下与宏
逐字节相同**（113,384 / 131,088 / 145,120 / 182,608 / 266,640），且比 v1 编译
更快（-O2 下 −6…−24%）。§2.2 的表现在承载三模式数字；之前宏/v1 两模式表的行
被同一会话的值取代。

**精确复现的论断**（稳定事实）：
- 诊断计数：238 vs 376 行，27,987 vs 42,478 字节，首条错误相同；概念输出里的"为什么"
  那几行。
- unit-serialization.cpp 在全部标准下的 `.o` 大小（2,555 / 2,580 / 2,595 /
  2,612 / 2,628 KB）—— 与上一版报告的十进制 KB 数值一致。
- §2.4 旧 vs 新 m2_diff：可执行文件逐字节相同（172,640 B / 27,600 B）、RSS ~354 MB、
  编译在噪声内、ASan 干净。
- §2.3：真实 `json_value` 的 8 个成员、仅 consteval 可枚举的陷阱、splice 用法 ——
  探针通过。
- §1.4：alt-string 参数顺序陷阱与 `<T, B>` 修复（最小复现）。
- 全部 16 个探针按各自记录的构建方式通过；`concepts_smoke` 在 c++11/20/26 下输出一致。

**未复现、并已修正的论断**：
- §1.2 分解：概念切片 −5% → 现为 ≈0%；`-freflection` 标志 +7–8% → +3.2%；标准升级
  +3% → +9.6%。墙钟绝对值也变了（3.77/3.74 s → 3.55/3.59 s）——机器环境使然；
  比值落仍在同一个"≈0 总成本"结论里。
- §2.2 反射-对-宏数字（编译、二进制、盈亏平衡）：旧编译单元从未入库，所以那里没有任何
  可复现的东西；改由入库的 `bench_macro_vs_reflection.cpp` 加完整的 N × 优化级别扫描
  代替。盈亏平衡从"28 行 / 28 类型"变成"25 行 / 25 类型"（序列化器的实际行数）。
- §2.2"二进制差距在 -O2 坍缩（+90% → +12%）"和"符号数相同（8）"：未复现——在可观测
  的代码生成下，-O2 差距在规模化时*更大*（N=100 时 exe +58%）；`nm` 显示反射符号更多
  （+10–26%）。
- §2.2 Q4"宏在运行时静默失败"：已修正——对已列出的不可序列化成员，两条路径都是编译
  错误；真正静默的失败是宏的*遗漏*，而反射做不到遗漏。

这篇文章的意义就在于这个教训：对于长时间进行中的评估，要保留原始运行记录和确切的
编译单元——散文形式的结论会存活，数字却不会。
