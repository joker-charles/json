# M4D：tagged-union API 面补全 —— at/erase/clear/swap/compare + value_t 名称/权重表

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验工作）
> **工具链**：GCC 16.1.0（`g++-16`），`-std=c++26 -freflection`
> **产物**：`include/nlohmann/reflection_json.hpp` + `tests/static-reflection/m4d_api.cpp`
> **状态**：已落地并通过全部差分测试（1411 项检查，ASan 无泄漏）

## 1. 它是什么

M2/M3/M4C 只把 tagged-union 的**存储层**（construct/destroy/invariant）与
**序列化层**（dump / CBOR / MSGPACK / UBJSON / BSON）反射化了；`basic_json`
的**其余成员函数**——`type_name`、`at`、`erase`、`clear`、`swap`、六个比较
运算符——在 `basic_json_reflection` 里完全缺失。主库 `json.hpp` 里这批函数
仍是手写 switch（`json.hpp` 18 处 + `iter_impl.hpp` 12 处 switch 的清点见
`FEASIBILITY.md` §1）。

M4D 把这一层补全，并落地 FEASIBILITY §2.2 的路线 a（标识符键控 consteval
权重表，repro_m1.cpp 已实证但从未进入真实头文件）：

1. **value_t 名称表 + 权重表**（`kValueTNames` / `kValueTTypeNames` /
   `kValueTWeights` + `value_t_order` / `value_t_less` / `value_t_weight`）：
   从反射枚举集生成，替代对主库手写 `value_t.hpp order[]` 与 `type_name()`
   switch 的依赖。`discarded` 权重 -1（unordered），保持 10→8 偏映射。
2. **API 面补全**：`type_name()`、`size()`、`empty()`、`at`（数组下标 +
   对象键，const/非 const）、`erase`（对象键 + 数组下标）、`clear()`
   （`template for` 表驱动）、`swap`（成员 + ADL friend + array/object/
   string/binary/container 容器形式）、六个比较运算符（== != < <= > >=）。

## 2. value_t 名称/权重表（FEASIBILITY §2.2 路线 a 落地）

主库 `value_t.hpp` 的 `order[]`（9 项）与 `json.hpp type_name()` switch 都是
手写、且分散在两处。M4D 用 **identifier-keyed consteval 函数**（repro_m1.cpp
实证模式）从 `enumerators_of(^^value_t)` 生成三张平行表：

| 表 | 内容 | 语义 |
|---|---|---|
| `kValueTNames` | 10 个枚举器标识符 | 与反射标识符逐一相等（static_assert + 差分） |
| `kValueTTypeNames` | 10 个显示名 | `number_*` → `"number"`，其余 = 标识符（json.hpp type_name 语义） |
| `kValueTWeights` | 10 个权重 | `null=0, boolean=1, number_*=2, object=3, array=4, string=5, binary=6, discarded=-1`（value_t.hpp order[] 语义） |

运行时 helper：

- `value_t_weight(t)`：查表，`discarded` 返回 -1；
- `value_t_order(a,b)`：`std::partial_ordering`，任一权重 <0 → `unordered`，
  否则按权重比较（等权 → `equivalent`，同 `value_t::operator<=>`）；
- `value_t_less(a,b)`：`value_t_order == less`（同 `value_t::operator<` 的
  is_lt 语义）。

`static_assert` 把表钉死在主库手写值上（`kValueTWeights[0]==0` …
`kValueTWeights[9]==-1`，`kValueTTypeNames[5..7]=="number"`），表长必须 == 10
（新增 value_t 枚举器 → 编译错误，绝不静默漂移）。差分测试做 10×10 全对
矩阵：`value_t_order` vs 库 `operator<=>`、`value_t_less` vs 库 `operator<`，
全部逐对相等（含 `discarded` 的 unordered）。

**偏映射说明**：`null`/`discarded` 在 union 里无成员（10 枚举 → 8 槽位），
权重表仍是 10 项——`discarded` 占一格但权重为 -1，`null` 权重 0 但无槽位。
这正是 FEASIBILITY §1「10→8 偏映射，不是满射」的表驱动形态。

## 3. API 面补全（对照主库语义逐条复刻）

### 3.1 type_name()

`kValueTTypeNames[m_type]`（越界回退 `"invalid"` 同主库 default）。
替换手写 switch。差分：10 种 value_t 全部与 `json::type_name()` 一致。

### 3.2 size() / empty()

主库语义（`json.hpp` 3.12）：`null → 0/true`；`array/object → 容器 size/
empty`；**其余全部 → 1/false**（string/boolean/数字/binary/discarded 都是
"单元素原子值"）。差分：10 种 value_t + 非空/空容器逐一对照。

### 3.3 at()

- `at(size_type)`：仅数组；越界 → `std::out_of_range`（容器 `at` 抛出，同
  主库 401）；类型错 → `std::runtime_error`（同主库 304）。
- `at(key_type)`：仅对象；键缺失 → `std::out_of_range`（403）；类型错 →
  `std::runtime_error`（304）。
- 返回 `json&`（存于我们 object/array 容器里的真 `nlohmann::json`），经
  `at` 的写操作直接进容器——与主库 `set_parent` 之外的语义一致。

### 3.4 erase()

- `erase(key_type)`：仅对象，返回删掉的个数（0/1），同主库
  `erase_internal`（`has_erase_with_key_type` 的 find+erase 路径）。
- `erase(size_type)`：仅数组，越界 → `std::out_of_range`（401）。
- **迭代器形式未实现**：`erase(iterator)`/`erase(first,last)` 需要
  `iter_impl.hpp` 那 12 处 switch 的迭代器里程碑（本里程碑范围外，文档化）。

### 3.5 clear() —— `template for` 表驱动

与 construct_one/destroy_one 同构：`template for (kValueTInfos)` +
每枚举动作 `clear_one<V>()`（数字 → 0、boolean → false、string/binary/
array/object → 容器 clear、null/discarded → no-op）。主库的 8-case switch
在反射版里是「新枚举器自动进入分派」的形态。

### 3.6 swap()

- `swap(reference)`：`std::swap(m_type, m_value)`——真 `json_value` 全部成员
  平凡（指针/标量），union 交换是位级搬运，与主库对自身 `json_value` 的
  `std::swap` 完全一致（主库 `json.hpp:3545` 同样这么干）；noexcept 成立。
- ADL friend `swap(left, right)`。
- `swap(array_t&)`/`swap(object_t&)`/`swap(string_t&)`/`swap(binary_t&)`/
  `swap(binary_t::container_type&)`：类型校验（错 → `std::runtime_error`，
  同主库 310）+ 容器内容交换。

### 3.7 比较运算符（== != < <= > >=）

逐条复刻主库 `JSON_IMPLEMENT_OPERATOR` 的**可观察语义**（`json.hpp`
3660-4030），非 legacy 模式（`JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON`
未定义）：

- 同类型：容器/字符串用容器 `==`/`<`，数字按类型直接比，`null` 定值
  （`==`→true、`<`→false），`discarded` → false；
- 跨类型数字：int/uint/float 六种组合全部走主库分支——**int/uint 用
  符号检查 + 无符号转换**（负数恒小于无符号，不丢精度），float 交叉用
  `static_cast<number_float_t>`（有损，与主库一致，差分钉死）；
- `compares_unordered`：NaN 数字 vs 任何数字 → unordered；任何 discarded →
  unordered（`<=`/`>=` 的 inverse 标志只在 legacy 模式有意义，未复刻）；
- **默认结果**（跨类型且非数字）：用反射生成的 `value_t_less`，而非主库
  手写 `order[]`——这是本里程碑的反射贡献点。

差分：30 个代表值（null/布尔/全整数宽度含 uint64 max 与 int64 min/max/
NaN/±inf/±0.0/字符串/数组/对象/binary/discarded）的全对矩阵 900 对 × 6
运算符，全部与主库一致；每值 dump 也逐字节一致（顺带暴露并修复了 dump 对
±inf 的潜伏漂移，见 §5）。

## 4. 错误通道（决策点 C 的延续）

主库抛 `nlohmann::detail::{type_error,out_of_range}`（仅继承
`std::exception`）；本研究库沿用 M4B 的**标准异常**惯例：类型错 →
`std::runtime_error`，越界/键缺失 → `std::out_of_range`。差分测试按
**分类 tag** 对照（type_error ↔ std::runtime_error，out_of_range ↔
std::out_of_range），不追求消息/错误码逐字一致（文档化边界，同 BSON 读方向）。

## 5. 顺带修复：dump 对 ±inf 的潜伏漂移

`reflection_serializer::dump_float` 原把 ±inf 写成 `"1e+999"`/`"-1e+999"`，
但主库 `serializer.hpp::dump_float` 是
`if (!std::isfinite(x)) { write "null"; }`——NaN **和** ±inf 都输出 `"null"`
（3.12 行为）。m3_dump 当年只覆盖 0.0/1.0/0.5/2.5，没盖到 inf，漂移潜伏至今；
M4D 的比较矩阵首轮差分即暴露。已修（`!isfinite` 单分支），并给 m3_dump 补
上 +inf / -inf / NaN / -0.0 四个回归用例。

## 6. 验证

- `tests/static-reflection/m4d_api.cpp`：1411 项检查全部 PASSED，含——
  10×10 权重/排序差分；type_name/size/empty 全枚举；at/erase/clear/swap 的
  快乐路径逐字节 dump 对照 + 错误路径分类 tag 对照；900 对比较矩阵；
  200 轮 construct/destroy/swap/clear 循环（ASan 无泄漏）。
- 既有回归全绿：m2_diff / m3_dump（含新增非有限用例）/ m3_binary /
  m4_binary / m4b_bson_reader / repro_m1 / probe_access / probe_union /
  probe_splice / probe_real_json_value / probe_reflection_replace_macros
  （M5 零漂移双编译）/ probe_enum_reflection（M6 零漂移双编译）。

## 7. 边界与后续

- `erase(iterator)` / `erase(first,last)` / `operator[]` / `value()` /
  `count` / `contains` / `find`：留给迭代器里程碑（iter_impl 12 处 switch）。
- 顶层 `std::variant` 支持仍未做（M5 §8 的既定边界）。
- `swap(binary_t&)` 等容器形式已实现，但 `operator[]`（null → 隐式转型
  容器）未做——`basic_json_reflection` 保持显式构造语义。
