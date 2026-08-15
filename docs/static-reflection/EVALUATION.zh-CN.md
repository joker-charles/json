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
通过 `-DBENCH_REFLECTION` 切换两种模式：

- **宏版本**：`NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_i, name, ...)` —— 每个结构体
  内一条宏调用
- **反射版本**：一个基于 `nonstatic_data_members_of` 的通用 `to_json`/`from_json`；
  用户类型**零**声明

`-DBENCH_N`（默认 50）控制 100 个已定义类型中有多少被 `main()` 实例化（未使用的
类型不生成代码）。两种模式都用相同的标志编译（`-std=c++26 -freflection`），且
`main()` 保持序列化器可观测（其输出喂给一个打印用的计数器），因此 `-O2` 无法消除
生成的代码——这是刻意的设计选择，见下面的注意事项。

反射序列化器（一次性成本；探针里的 `namespace refl` 块是 **24 行**，加 1 行
`#include <meta>` = 25 行）：

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

**构建过程中踩到的陷阱（全部都是已验证的 GCC 16 行为）**：
- `std::meta::` 必须完整限定；`std::define_static_array`（不是
  `std::meta::define_static_array`）
- `nonstatic_data_members_of` 返回一个**临时 vector**：直接在调用上取下标/取大小，
  千万不要绑定到局部 `constexpr`
- `template for` 的范围要么内联，要么是命名空间作用域的 constexpr
- 嵌套类成员的递归序列化**不会**落入 nlohmann 的 ADL 重载集（`detail::to_json` 不是
  裸函数）；标量必须走赋值 / `.template get<M>()`
- `if constexpr` 不会丢弃 false 分支 ⇒ 类-对-标量递归需要 tag 分发

### 2.2 结果

**Q1 节省的代码**（每种类型的用户行数：宏 = 结构体 + 宏调用 = 2/类型；
反射 = 仅结构体 = 1/类型；一次性序列化器 = 25 行）：

| 类型数 | 宏（行） | 反射（行） | 净值 |
|---|---|---|---|
| 1 | 2 | 26 | −24 |
| 20 | 40 | 45 | −5 |
| **25** | 50 | 50 | **盈亏平衡** |
| 28 | 56 | 53 | +3 |
| 50 | 100 | 75 | +25 |
| 100 | 200 | 125 | +75 |

反射每类型**省 1 行**，但需要一个**25 行的一次性通用序列化器**；超过约 25 个
类型才开始净赚。

**Q2 编译时间**（同一编译单元、同一 `-std=c++26 -freflection`，取 3 次最小值）：

| N | 宏 -O0 | 反射 -O0 | Δ | 宏 -O2 | 反射 -O2 | Δ |
|---|---|---|---|---|---|---|
| 1 | 2.22 s | 2.38 s | +7% | 3.11 s | 3.24 s | +4% |
| 20 | 2.43 s | 2.58 s | +6% | 3.27 s | 3.89 s | +19% |
| 28 | 2.47 s | 2.73 s | +11% | 3.38 s | 4.16 s | +23% |
| 50 | 2.64 s | 2.92 s | +11% | 3.79 s | 4.99 s | +32% |
| 100 | 3.13 s | 3.60 s | +15% | 5.13 s | 7.21 s | +41% |

**Q3 二进制**（各 N 下可执行文件字节 / `size` text）：

| N | 宏 -O0 | 反射 -O0 | Δ exe / Δ text | 宏 -O2 | 反射 -O2 | Δ exe / Δ text |
|---|---|---|---|---|---|---|
| 1 | 393,656 / 169,213 | 484,512 / 199,086 | +23% / +18% | 113,384 / 80,111 | 119,888 / 85,244 | +6% / +6% |
| 20 | 445,936 / 202,548 | 555,608 / 260,100 | +25% / +28% | 131,088 / 94,192 | 174,456 / 129,294 | +33% / +37% |
| 28 | 470,560 / 216,580 | 584,264 / 285,788 | +24% / +32% | 145,120 / 106,241 | 194,248 / 149,994 | +34% / +41% |
| 50 | 530,088 / 255,186 | 669,208 / 356,438 | +26% / +40% | 182,608 / 138,326 | 265,248 / 207,914 | +45% / +50% |
| 100 | 667,600 / 343,012 | 850,360 / 517,124 | +27% / +51% | 266,640 / 208,925 | 421,720 / 342,974 | +58% / +64% |

**优化级别敏感性（已修正的说法）**：旧报告声称 -O0 的膨胀"到 -O2 就蒸发了"
（+90% → +12%）。在一个诚实地保持序列化器可观测的基准里，-O2 差距**不会**坍缩——
对小 N 它更小（N=1 从 -O0 的 +23% 降到 +6%），但它**随 N 增大**，最终在 -O2 下
*更大*（N=100 时 exe +58% vs -O0 的 +27%）。旧报告的坍缩是它那未入库编译单元的
产物——其 -O2 构建显然把两个序列化器的大部分都消除掉了。凡是报告编译期/二进制
数字，都要同时说明优化级别**以及**生成的代码是否可观测。

膨胀归属（在两个优化级别都成立）：差值在 **.text（指令）而非 rodata**——两种模式下
rodata 都约为 1.7 KB（-O0）/ 1.3 KB（-O2），所以逐成员的 `std::string(identifier_of(m))`
键并不是开销来源；反射路径每条成员生成更多代码（而且符号更多：在 N=50 时 `nm`
计数为 -O0 下 1,471 vs 1,621（+10%）、-O2 下 238 vs 300（+26%）。text 比符号增长得
更快，因此是逐实例化的代码，而不是符号膨胀）。

**Q4 诊断** —— 用刻意构造的坏类型（`std::mutex` 成员）验证：

| 场景 | 宏 / 手写 | 反射 |
|---|---|---|
| 成员在宏里**列出**但不可序列化 | 编译错误（`no match for 'operator=' ... const std::mutex`），错误指向宏展开内部（`macro_scope.hpp:580`） | 编译错误，同样的底层错误出现在序列化器的 `j[...] = v.[:m:]` 行；两者都点名 `std::mutex` |
| 成员在宏里**没列出** | **静默**：`NLOHMANN_DEFINE_TYPE_INTRUSIVE(with_mutex, id)` 能编译，序列化出 `{"id":42}` —— `mutex` 成员被静默丢弃，往返有损，且无任何诊断 | 不可能：通用序列化器反射**所有**成员，所以 mutex 会成为编译错误 |

**解读（已修正）**：旧报告声称宏会*在运行时静默失败*、而反射会在编译期失败——针对
同一个不可序列化成员。这并未复现：当成员在宏里列出时，两条路径**都是编译错误**并
点名出错的类型。真正的不对称是**静默遗漏**：宏可能忘掉一个成员，于是在零诊断的
情况下产生有损的序列化；反射不可能遗漏任何东西，所以它会在编译期大声失败。这——
而不是"已列出成员的错误质量"——才是反射真正的诊断优势。

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

# 反射 vs 宏（已入库的基准编译单元，见 §2.2）
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
  -o /tmp/bm tests/static-reflection/bench_macro_vs_reflection.cpp      # macro
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_REFLECTION -Iinclude \
  -o /tmp/br tests/static-reflection/bench_macro_vs_reflection.cpp      # refl
size /tmp/bm /tmp/br && nm /tmp/bm /tmp/br | wc -l

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
