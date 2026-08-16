# 静态反射（P2996）重写 nlohmann/json — 可行性分析与设计方案

> 分支：`feature/static-reflection`（基于 `develop`，cdf52ae9）
> 编译器基准：`g++-16` 16.1.0（Ubuntu 16.1.0-2ubuntu1）/ libstdc++ 16，`-std=c++26 -freflection`
> 状态：**M0–M3 完成并实机验证；M4 反射不推进、concepts 化分层落地到主库 to_json；M5 注解驱动的双向反射序列化器落地（替换 NLOHMANN_DEFINE_TYPE_* 宏族，场景 B）；M6 枚举字符串映射落地（替换 NLOHMANN_JSON_SERIALIZE_ENUM）**。M0–M3 见下；M4 见 `docs/static-reflection/M4_ASSESSMENT.md`；M5/M6 见 `docs/static-reflection/M5_REFLECTION_TO_JSON.md`。M4 已有：`concepts.hpp` 层 1/2 概念（原子探测 + 带 BasicJsonType 语义概念）、to_json 浮点/枚举/字符串/数组/对象 5 个重载 `JSON_HAS_CPP_20` 双轨 `requires`，五标准行为一致、差分与全部 fixture 零回归；range 视图维度以变量模板门控（不入 concept）；from_json 保留 enable_if。复合约束可行性实证见 §6/§7。M5 已有：`include/nlohmann/reflection_to_json.hpp`（refl2 codec 迁入 + `json_name`/`json_ignore`/`json_default` 注解层 + 双向 catch-all），门控于 `__cpp_impl_reflection && __cpp_lib_reflection`（仅 g++-16 `-std=c++26 -freflection`），宏↔注解差分探针、零漂移双编译、8 编译期负例、doctest 套件全部通过。M6 已有：枚举器级 `json_name` 注解（属性须在标识符后）+ 编译期枚举器表（枚举器 splice 取值）+ 双向字符串映射，`to_json`/`from_json` enum 重载三态门控，无注解枚举保持整数路径零漂移。

---

## 0. 目标与非目标

- **目标**：探明 P2996 静态反射在本库中"哪些点真正受益、怎么落地、会用 GCC 16 的哪些已验证模式"，产出一份可执行的实现路径。
- **非目标**：在本阶段不写库级实现、不破坏既有 C++11 API、不触碰 `single_include` 生成物。

---

## 1. 库的核心数据结构（反射的天然靶点）

`basic_json` 本质是一个 **带判别式的 tagged union**：

1. **`enum class value_t : std::uint8_t`**（`detail/value_t.hpp`，10 个枚举值）：
   `null, object, array, string, boolean, number_integer, number_unsigned, number_float, binary, discarded`。
   位于 `nlohmann::detail` **命名空间作用域、公有**——可直接反射，无需镜像。

2. **`union json_value`**（`include/nlohmann/json.hpp:456`，8 个可存储成员，全部为**非静态数据成员**）：
   - 指针类（可空、需手动分配/销毁）：`object_t* object`, `array_t* array`, `string_t* string`, `binary_t* binary`
   - 标量类（值语义、内联）：`boolean_t boolean`, `number_integer_t number_integer`, `number_unsigned_t number_unsigned`, `number_float_t number_float`
   - **注意**：`json_value` 是 `basic_json` 的 **private 嵌套类型**（456 行处于 `private:` 之后）；`struct data`（`:4272`）同样私有。外部反射枚举它们需要 `access_context::unchecked()`（§4.2 已实证可行）。

3. **`struct data`**（`:4272`）持有判别式 `value_t m_type` 与联合体 `json_value m_value`。

4. **每类值都被 `m_type` 与联合体当前活跃成员约束** —— 库里有 **大量** 手写的、跨 8~10 分支的类型分发，且同一性约束（某 `value_t` ⇄ 某 union 成员）在 **25 处**（json.hpp 12 + iter_impl.hpp 12 + serializer 1，另 binary_writer/reader 多个 switch 与手工表）**重复手写、极易漂移**。

### 手写 switch 分发点的实际规模（本次核查，非约数）

| 位置 | 内容 | 分支数 |
|---|---|---|
| `json.hpp`：`switch (m_data.m_type)` **11 处** + `switch (parent.m_data.m_type)` 1 处 | `get_/at/erase/clear/swap/compare/…` | 8–10 |
| `detail/iterators/iter_impl.hpp` | `switch (m_object->m_data.m_type)` **12 处** | 8–10 |
| `detail/output/serializer.hpp::dump` | 序列化分发 | **9 个 case**（object/array/string/binary/…/discarded） |
| `union::json_value(value_t)` 构造（`:486`） | 按类型建默认值 | 10 |
| `union::destroy(value_t)`（`:587`） | 按类型释放指针成员 | 4 指针 case + default（标量无需释放） |
| `value_t::operator<=>` / `operator<`（`value_t.hpp:86-91`） | 手工 `order` 数组 **9 项**（`discarded` 不参与比较 → unordered） | — |
| `detail/output/binary_writer.hpp` / `detail/input/binary_reader.hpp` | CBOR/MessagePack/UBJSON/BSON 类型字节表与往返 | 多个 switch + 手工表 |
| `exceptions.hpp` | 5 个 `exception` 派生类的手写重复 `create()` | — |

**映射特例（M2 必须处理）**：`null` 与 `discarded` 在 `json_value` 里**没有对应成员**，两者都落回 `object = nullptr`（构造 `:538-553`）。所以"`value_t` ⇄ union 成员"是 10→8 的偏映射，不是满射。

---

## 2. 静态反射能带来哪些真实收益（按性价比排序）

### 2.1 ★★★ 用反射生成"枚举值 ⇄ 成员"的编译期映射表，消解所有手写 switch

`value_t` 与 `union json_value` 之间的对应关系，目前**在构造、销毁、序列化、反序列化、比较等十几处各自硬编码**。新增一个 `value_t` 或一个 union 成员时，所有 site 都要同步改，漏一处就是潜伏 bug。

P2996 可以在编译期枚举并建立该映射：

- `meta::nonstatic_data_members_of(^^json_value, ctx)` → 8 个 union 成员及其名称/类型（**只含数据成员**，构造/`destroy` 等成员函数不会混入——已实证）；
- `meta::enumerators_of(^^value_t)` → 10 个枚举值。

**已实机验证的模式**（`tests/static-reflection/` 夹具 + 探针）：

- **union 反射**：`nonstatic_data_members_of` 对 union 类型生效，返回成员数量与声明序；`type_of(m)` + `std::is_pointer_v<T>` 可把成员分成"指针类/标量类"两类（探针输出 `is_pointer=1/0`）。
- **编译期名称/权重表**：`std::index_sequence` 打包展开 + **对调用直接下标**（`enumerators_of(^^T)[i]`），不依赖 `template for`——这是最稳的建表方式（`repro_m1.cpp` 实证）。
- **`template for` 仅 range 形式**：`template for (constexpr auto r : define_static_array(...))`，range 必须是内联表达式或 namespace-scope/static constexpr；循环体内可直接 splice（`auto e = [:r:]`、`v.[:r:]`、`typename [: type_of(r) :]`）。
- **值 splice**：枚举值先 `auto e = [:r:]` 再 `static_cast`（`extract<int>` 对枚举抛异常）。

映射表骨架（索引在编译期求得，对调用直接下标）：

```cpp
template<value_t V> struct trait_from_value_t;            // 部分特化/标签派发，勿放 if constexpr 假分支
constexpr std::size_t index_for = /* trait_from_value_t<V>::index，编译期常量 */;
constexpr auto m = meta::nonstatic_data_members_of(^^json_value,
                     meta::access_context::unchecked())[index_for];   // 直接下标，不绑局部
using M = typename [: meta::type_of(m) :];                // 成员类型
bool is_ptr = std::is_pointer_v<M>;                       // 存储类别
```

**必须规避的 GCC 16 坑**（skill 已验证 + 本次实证）：
- `if constexpr` **不会**丢弃假分支 —— 统一用 **标签类型重载 / struct 部分特化** 分发。
- 局部 `constexpr auto es = define_static_array(...)` 会报 "address ... add 'static'"；`constexpr auto es = enumerators_of(...)` 直接报 "refers to a result of 'operator new'" —— 要么内联使用，要么 namespace-scope `constexpr` / 先用 `constexpr size_t n = es.size()` 带出。
- 不要把 `template for` 循环变量按值传入 `consteval` helper（用 `template <meta::info r>` NTTP）。

### 2.2 ★★★ 反射生成 `value_t` 的类型名 / 稳定排序

`value_t::operator<=>/<` 现在手工维护 `order` 数组（`value_t.hpp:86`，**9 项**：null=0, boolean=1, number_*=2, object=3, array=4, string=5, binary=6；`discarded` 不在表内 → unordered），还要照顾 GCC 的 `<=>` 重写选择坑（bug 105200，`value_t.hpp:106-115` 注释）。

两条实现路线（`repro_m1.cpp` 已实证路线 a）：

- **a. 标识符键控 consteval 函数**（推荐）：`consteval int sort_weight(std::string_view id)` 按枚举名返回权重，`index_sequence` 展开成编译期表。**直接作用于真 `value_t`**（公有可反射），不依赖注解，零 P3394R4 陷阱；新增枚举值时只需改一个 consteval 函数。
- **b. 注解驱动**：`[[=rpb::weight<N>{}]]` 挂在枚举值上。可行（`annotations_of` 支持枚举值），但需要**重声明一个带注解的镜像枚举**（主库 `value_t` 不能动，C++11 语法装不下注解），且要处理 §4.4 的全部注解读取陷阱——性价比不如 a。

两种路线都必须保持与主库**逐字节等价**的 `partial_ordering` 语义（含 `discarded` 的 unordered 与 GCC 的 `<` 特判）。

### 2.3 ★★ 序列化/反序列化的类型分发矩阵

`serializer::dump`（9 个 case）、`binary_writer`、`binary_reader` 的核心都是类型分发。M3 已在 `reflection_json.hpp` 中用反射驱动实现 JSON 文本 dump 与 CBOR 写出：值为分发用 **`template for(kValueTInfos)` + 每枚举 NTTP 动作（`dump_one<V>`/`cbor_one<V>`）**，替换手写 `switch (type)`；各二进制格式类型字节表（CBOR/MessagePack/UBJSON/BSON 各自给 `value_t` 一个字节码）同理可各化为一张 consteval 码表。这正是 skill「Reflection-driven serialization」里"反射驱动 wire codec"的正规用法。**已覆盖（M3 + M4B）**：CBOR（`reflection_cbor_serializer`）、MSGPACK/UBJSON（基础模式）/BSON（`reflection_msgpack_serializer`/`reflection_ubjson_serializer`/`reflection_bson_serializer`，主字节码来自反射生成的 `kMsgpackCodes`/`kUbjsonCodes`/`kBsonCodes` 表，值相关分级在每枚举动作内）——差分测试 `m3_binary.cpp`/`m4_binary.cpp` 与真库逐字节一致。**BSON 读入已覆盖（M4B-2）**：`reflection_bson_parser`（rjson 镜像的读方向，与 `reflection_bson_serializer` 对称），元素类型分发来自反射生成的 `kBsonsLoad` 反向表（字节码 → {union 槽位, 载荷形态}），差分测试 `m4b_bson_reader.cpp` 与真库 `from_bson` 逐字节一致。**尚未覆盖**：CBOR/MSGPACK/UBJSON 读入、UBJSON 优化模式（`'#'`/`'$'` 前缀）与 BJData。

### 2.4 ★★ `is_*` 类型判定与断言去重

`assert_invariant`、`value_t` 关系等都可收敛为：反射枚举 union 成员，**自动获得"哪些类型存指针、哪些存标量"**（`is_pointer_v<type_of(m)>`，已实证），从而自动生成 `is_object()/is_string()/...` 这一族与 `assert_invariant` 的检查体，避免手工罗列。

### 2.5 ★ `type_traits.hpp` 里的 SFINAE/detected 手写 trait

`include/nlohmann/detail/meta/type_traits.hpp`（959 行，已核实）满是手写 `is_detected` / `enable_if` / `conjunction` 样板。**但 M4 前置评估（见 `docs/static-reflection/M4_ASSESSMENT.md`，含实证）表明此前的乐观判断需修正**：逐 trait 盘点后，绝大多数（约 20 个，`is_compatible_*` / `has_to_json` / `is_range` / `is_transparent` / `is_c_string` …）是**外部任意用户类型的可替换性探测**——这恰是反射的死角（需 `void_t`+`decltype` 的 SFINAE 软失败语义），**不能**用反射替代。可替代的仅 A 类约 6 个本库类型判定（`is_basic_json`/`is_json_ref` 等），且它们本就稳定、无漂移源，替换只算风格表演；`is_specialization_of` 的反射替代已被 `repro_traits.cpp` 实证**技术上部分可行但当前受 `template_arguments_of` transient-vector 限制、且无实际收益**。**故不建议推进完整 M4**（详见评估文档 §3 建议）。

---

## 3. 最大的约束与冲突：C++11 契约 vs 反射

贡献指南（`.github/CONTRIBUTING.md`）明确：
- 库要保持 C++11 兼容、**单头文件**、**不得破坏公共 API**（语义版本 3.x.y）。
- 但 **P2996 是 C++26 独占**，`-freflection` 只有 GCC 16 支持，且把反射代码塞进主 `json.hpp` 会让所有 C++11/14/17/20 用户无法编译。

### 结论性建议

反射重写**不可能也不应该**落在主 `json.hpp` 的既有 C++11 路径内。它应当作为一个**独立、可自由实验**的产物，与本库主路径解耦：

1. **全新独立的 C++26 单头** `include/nlohmann/reflection_json.hpp`（或 `single_include` 下），API 语义复刻 `basic_json`，内部用反射生成的 tagged-union 分发表。运行期行为与主库**逐字节等价**（做差分测试）。
2. **不可改**现有 `include/nlohmann/*` 的 C++11 语义，也不动 `single_include` 生成物。
3. `make amalgamate` 目前针对主库 C++11 路径；反射版不必走 amalgame 生成链（或单独脚本处理反射版）。
4. 若未来希望合回主库，唯一通道是 `JSON_HAS_CPP_26` 特性宏 + 完整 C++11/23 回退——但那要同时保住 100% 测试覆盖，工作量极大，**不建议在实验阶段追求**。

**本次调研对该结论的补充**（§4 探针实证）：`value_t` 公有可直接反射（无需镜像）；`json_value`/`data` 私有，直接命名 splice 不可引用，但**经私有成员 `type_of` 间接获得完全可行**（§4.5，`probe_real_json_value.cpp` 实证）——用 consteval helper 可直接驱动真实私有内部结构，镜像 union 只是 `template for` 内联路径受限时的务实替代。独立头 `reflection_json.hpp` 当前按镜像路线实现：复用主库的公有类型别名但自持存储，运行期行为与主库逐字节等价（m2_diff.cpp 差分通过）。若升级为 §4.5 的 consteval 直接反射路线，独立头仍是最干净的差分对照载体，但镜像层可删。

这与贡献指南的"双轨特性宏（如 `JSON_USE_IMPLICIT_CONVERSIONS`）"精神一致，只是反射版作为实验分支独立演进。

---

## 4. 已验证的 GCC 16 反射模式清单（实现必须遵守）

编译基准已验证（本机，`g++-16` 16.1.0 / libstdc++ 16，2026-08）：

### 4.1 已跑通的模式（可直接使用）

| 模式 | 实证 |
|---|---|
| 工具链冒烟：`printf 'int main(){}' \| g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t` | 通过 |
| `std::meta::` 命名空间（**必须全限定**，裸 `meta::` 不编译） | `repro_m1.cpp` 初版报错 → 修正后通过 |
| `enumerators_of` / `nonstatic_data_members_of` / `members_of` / `identifier_of` / `type_of` / `extract<T>` / `substitute` / `define_static_array/string/object`（完整查询索引见 skill） | `repro_m1.cpp` |
| **对调用直接下标/取 size**（transient-vector 规则：`enumerators_of(^^T)[i]`、`enumerators_of(^^T).size()`，绝不先绑局部） | `repro_m1.cpp` / `probe_access.cpp` |
| `std::index_sequence` + 打包展开生成编译期表（无需 `template for`） | `repro_m1.cpp` 输出 |
| range 形式 `template for (constexpr auto r : define_static_array(...))`，range 为内联表达式或 namespace-scope/static constexpr；循环体直接 splice | `repro_m1.cpp` 输出 |
| 值 splice：`auto e = [:r:]` 后 `static_cast`（枚举值） | `repro_m1.cpp` 输出（10 个枚举值全部正确） |
| union 反射：`nonstatic_data_members_of` 对 union 生效；`typename [: type_of(m) :]` + `is_pointer_v` 区分指针/标量 | `probe_union.cpp` 通过（5 成员 union：`pointer=1/0`） |
| **`access_context::unchecked()`**：外部反射可枚举 private 嵌套类型/成员（`unprivileged()` 下为 0，`unchecked()` 下可见全部） | `probe_access.cpp` 通过 |
| **私有成员 splice 读写**：用 `unchecked()` 拿到的 info 做 `o.[:m:]` 读、`o.[:m:] = v` 写，外部代码可读写 private 成员 | `probe_splice.cpp` 通过（`secret` 42→7） |
| P3394R4 注解读取：类型编码注解 `template<std::uint32_t N> struct w{ static constexpr auto value = N; }` + `[: type_of(ann) :]::value`；或 structural 值注解 + `extract<AnnType>(ann)`；读取前先 `remove_cvref_t` | skill 已验证 |
| 契约：`contract_assert(cond)`；需全局 `void handle_contract_violation(std::contracts::contract_violation const&)`，访问器是 `.comment()`（无 `.message()`）；header-only 库应带 weak 默认 handler（abort） | skill 已验证 |
| `#embed`：指令独占一行；**不搜 `-I`**，文件须同目录或绝对路径 | skill 已验证 |

### 4.2 access_context 三态（libstdc++ 16 实测）

`<meta>` 提供三种访问上下文（`/usr/include/c++/16/meta` 实测）：

| 上下文 | 行为 | 用途 |
|---|---|---|
| `current()` | 按当前词法上下文访问 | 类内/友元场景 |
| `unprivileged()` | 仅公有可见（skill 原记录） | 默认调研/公开类型 |
| `unchecked()` | 绕过访问控制，可枚举并 splice 读写在**求值语境**下的私有成员 | 类外查询私有成员 |

**M2 实证发现（§4.5 已进一步修正）**：`unchecked()` **不能**让外部 scope-splice **直接命名** `basic_json` 的 private 嵌套类型（`using V = [: ^^ json::json_value :];` 报 `is private within this context`）——splice 的类型域**直接命名**走普通访问检查，不经 access_context。但见 §4.5：**经由私有数据成员的 `type_of` 间接获得**该类型并完整使用是**可行**的，镜像 union 是当时（`template for` 内联路径受限时）的务实选择，**不是硬性要求**。`value_t`（`nlohmann::detail`，公有）可直接 `enumerators_of`；镜像与真实布局通过差分测试保证一致（`m2_diff.cpp`，逐字节等价）。

### 4.3 须规避（GCC 16.1.0 硬限制）

| 限制 | 替代方案 |
|---|---|
| `if constexpr` 不丢弃假分支（含假分支内失败的依赖实例化） | 标签类型重载 / struct 部分特化 |
| 局部 `constexpr auto es = enumerators_of(...)`（"refers to a result of 'operator new'"） | 对调用直接下标 / `define_static_array` |
| 局部 `constexpr auto es = define_static_array(...)`（"add 'static'"） | 内联表达式 / namespace-scope `constexpr` |
| 局部 `span` 在运行时是 consteval-only | 先用 `constexpr size_t n = es.size()` 之类把值带出 |
| `extract<int>` 枚举值抛异常（`extract` 仅支持字符串/对象） | `auto e = [:r:]` + `static_cast` |
| 循环变量按值传 `consteval` helper 失败 | `template <meta::info r> consteval` NTTP helper |
| `typename[:expr:]::` 在求值语境解析失败 | 作用域拼接形式或移到类型语境 |
| `annotations_of` 对类型返回空（下标会触发 hardening assert） | 先走 `nonstatic_data_members_of(...)[I]` |
| 裸 `meta::`（未限定） | `std::meta::` |
| `template for` 索引式写法（`template for (size_t i = 0; ...)`）**不存在** | 只用 range 形式 |
| `name_of` / `for_each` 缺失 | `identifier_of` / 自写循环 |
| `offset_of` 返回 `member_offset`（`.bytes`/`.bits`，`total_bits()`），**不是整数** | 比较 `.bytes` |
| `template_arguments_of` **含默认实参**；类型别名无 template 参数（抛异常） | 走底层类型 |
| `is_enum_type` / `is_class_type` 后缀命名（无裸 `is_enum`/`is_class`） | 用 `_type` 后缀版 |

### 4.4 注解读取（P3394R4 `[[=expr]]`）陷阱

- 注解 info **不能出现在 splice 表达式**里（`[: ann :].value` 被拒）。两条已验证读取路线：(a) 类型编码注解 + `template for` + `[: type_of(ann) :]::value`；(b) structural 值注解 + `extract<AnnType>(ann).value`。
- `type_of(ann)` 带 cv/ref 限定，匹配注解类型前先 `std::remove_cvref_t`。
- 传统属性与值注解**不能混在同一个 `[[...]]` 列表**（`[[nodiscard, =w<7>{}]]` 被拒），用独立列表；`annotations_of` 只计数值注解。
- `[[=expr]]` 值必须是 **structural 类型**。
- 类型级注解被 GCC 16 忽略（`[[=...]] struct S` 无效）——注解只挂成员/枚举值。

### 4.5 私有嵌套类型：直接命名失败，但**经成员 `type_of` 间接获得完全可行**（修正 §4.2/M2 定案）

`probe_real_json_value.cpp`（修正性探针，**PASSED**）推翻了"反射版无法驱动真实私有 `json_value`"的旧结论。精确边界：

| 操作 | 结果 |
|---|---|
| 直接命名 `[: ^^ json::json_value :]` | ❌ `is private within this context`（splice 直接命名走普通访问检查） |
| `unchecked()` 枚举 `basic_json` 私有数据成员 | ✅ 拿到 `data`（私有嵌套 struct，经 `m_data`） |
| `type_of(m_data)` → `data`；再枚举其成员 → `m_value` | ✅ 全部可达 |
| `type_of(m_value)` → 真实 `json_value` | ✅ 类型可 splice 声明（`using V = [: info :]`） |
| **consteval 函数**里枚举 `json_value` 的成员 | ✅ 8 个成员全列出 |
| `template for` **内联**里枚举同一类型 | ❌ `not a complete class type`（**GCC 16 关键陷阱**） |
| splice 构造 / 读写成员 / 分配释放指针成员 | ✅ 全通过（`value_t` 带参构造、`number_integer`/`string`/`boolean` 读写） |

**结论**：`json_value_mirror` 镜像 union 是**当时 `template for` 内联路径受限下的务实选择，不是硬性要求**。若用 **consteval helper 函数**（而非 `template for` 内联）做反射枚举，可直接驱动真实 `basic_json::json_value`——省掉镜像，也消除"镜像与真实布局漂移"风险（m2_diff 的差分负担随之消失）。§5 里程碑 3 的镜像路线可升级为"consteval 直接反射真实 union"。**已升级（M4C）**：`reflection_json.hpp` 已删除镜像层，`basic_json_reflection` 直接持有 splice 出的真实 `json_value` 作为存储载体（真实 union 无用户析构、成员默认公有、`json_value() = default` + `{}` 零初始化不分配——三条新事实见 `VERIFIED_FACTS.md`）；m2_diff/m3_dump/m3_binary/m4_binary/m4b_bson_reader 全部差分测试重跑逐字节等价、ASan 无泄漏。

---

## 5. 建议的落地顺序（里程碑）

1. **M0 编译基线 ✓**：分支 `feature/static-reflection` + 工具链冒烟（一行命令见 §4.1）。
2. **M1 反射化 `value_t` ✓**：`tests/static-reflection/repro_m1.cpp` 已跑通——
   `g++-16 -std=c++26 -freflection -O2 -o /tmp/repro_m1 tests/static-reflection/repro_m1.cpp`
   验证：枚举计数/名称表、标识符键控权重表（对主库 `order[]` 语义）、值 splice、range `template for` + `define_static_array`。产物：编译期"名称/权重"表，可直接替换 `operator<=>` 手写 order 表。
3. **M2 反射化 tagged union 分发 ✓**：前提已实证，产物已完成——
   - `tests/static-reflection/m2_table.cpp`：实证 `json_value` 私有、**直接命名** splice 不可引用（当时据此选镜像路线），生成 `value_t ⇄ 镜像成员 ⇄ 存储类别` 单源表；后续 `probe_real_json_value.cpp`（§4.5）证明经成员 `type_of` 间接获得 + consteval 枚举可行，镜像路线可升级为直接反射真实 union。
   - `include/nlohmann/reflection_json.hpp`：独立 C++26 头，`basic_json_reflection` 用 `kStorage`（反射表）+ `slot_index<V>` + `template for` 遍历全部 `value_t` 枚举驱动默认构造/destroy/invariant，**新增 value_t 未接线即编译错误**（完整性保证）。**M4C 已升级**：存储载体从 `json_value_mirror` 换成 splice 出的真实 `json_value`（镜像层删除）。
   - `tests/static-reflection/m2_diff.cpp`：差分测试 vs 主库逐字节等价（含 `is_number_unsigned` 也计为 integer 的语义），ASan 下 `destroy` 无泄漏。**PASSED**。
4. **M3 序列化表驱动 ✓**：`reflection_json.hpp` 新增两个反射驱动序列化器——
   - `reflection_serializer`：JSON 文本 dump。值分发由 `template for(kValueTInfos)` + NTTP `dump_one<V>` 驱动（无手写 switch on type），复刻对象/数组递归、字符串转义、数字格式、二进制结构、`<discarded>` 特殊输出。
   - `reflection_cbor_serializer`：CBOR 二进制写出。整数/主类型宽度选择、compact-float 前缀、容器长度头、binary subtype(tag 在前, 字节串 major 2) 全部复刻主库。
   - 差分测试 `tests/static-reflection/m3_dump.cpp` 与 `m3_binary.cpp`：覆盖 10 个默认 value_t、整数宽度边界（0/23/24/255/256/65535/65536、负数边界）、浮点（0.0/1.0/0.5/2.5）、嵌套容器、二进制带/不带 subtype、ensure_ascii 转义，**逐字节等于主库 `dump()`/`to_cbor()`**，ASan 无泄漏，全部 PASSED。
5. **M4 type_traits —— 反射不推进，concepts 化分层落地**：见 `docs/static-reflection/M4_ASSESSMENT.md`。反射侧：绝大多数（~20 个）是外部类型探测，反射不可替代；A 类（~6 个）本就稳定，替换无收益（`repro_traits.cpp` 实证"能表达但不落地"）。concepts 侧已**落地到 to_json.hpp**：
   - **层 1/2**（`concepts.hpp`）：原子探测概念 + 带 `BasicJsonType` 的语义概念（`array_like`/`object_like`/`string_like`），以 `probe_layered_concepts.cpp` 零漂移为基准。
   - **层 3**（to_json 重载）：浮点/枚举/字符串/数组/对象 5 个重载改为 `#ifdef JSON_HAS_CPP_20` 双轨 `requires` 排他内联；`range` 视图维度用 `not_range_view` 变量模板门控（不入 concept，防循环约束）。
   - **验证**：C++11/14/17/20/26 五标准 roundtrip 输出逐字节一致；`vector<uint8_t>`→array、string→string、map→object、枚举/UDT 全部正确（排他语义等价）；全部 fixture + 差分零回归。
   - **from_json 保留 enable_if**：其 array/object 用 `is_constructible_*`（反向语义）+ `priority_tag` 分派，概念化风险高收益低（§6 结论）。
6. **全程差分测试方法论**（skill「Reflection-driven serialization」测试章节）：字节级差分测试 vs 主库（对照实现）；**一文件一论断**的最小 repro 验证每个编译器行为论断；记录"合法但不同"的偏差（如 packed/unpacked 输出形式）并显式归档。

---

## 6. 关键决策点（待确认）

- **A. 是否作为"独立 C++26 头"实现**（推荐），还是坚持"主库内 `JSON_HAS_CPP_26` 宏双轨"（代价是 C++11 契约 + 100% 覆盖压力）。本次调研新增证据：`unchecked()` 可驱动真实内部结构，但独立头仍是唯一干净的差分对照载体。
- **B. 反射版 API 是否需要 100% 复刻**（推荐：是，便于差分测试），还是允许精简。
- **C. 错误处理风格**：复刻 `json.exception` 体系（API 兼容、便于差分），还是 `std::expected` + `contract_assert`（skill 的 no-exceptions 路线，省空间但偏离主库抛错语义）。若用契约，需按 §4.1 提供 weak 默认 handler。
- **D. schema 元数据载体**：consteval 标识符键控表（推荐，2.2a 已实证），还是 P3394R4 注解（2.2b，陷阱见 §4.4，需镜像枚举）。
- **E. 是否重声明带注解的 `value_t` 镜像**：不推荐——真 `value_t` 公有可直接反射，镜像只会引入"两套枚举漂移"风险。

---

## 7. 参考与复现

- 本文件供 `feature/static-reflection` 分支后续实现引用；完整已验证事实汇总在 `modern-cpp` skill（头文件速查、API 签名、反射模式、注解读取、wire codec 设计）。
- 夹具（`tests/static-reflection/`）与产物：
  - `repro_m1.cpp` —— M1 枚举反射（名称/权重表 + 值 splice + `template for`），跑通。
  - `probe_access.cpp` —— `unchecked()` 访问控制探针（私有成员枚举），跑通。
  - `probe_union.cpp` —— union 成员反射与指针/标量分类，跑通。
  - `probe_splice.cpp` —— `unchecked()` 私有成员 splice 读写，跑通。
  - `m2_table.cpp` —— M2 建表探针：实证 `json_value` 私有**直接命名** splice 不可引用 → 当时选择镜像路线，生成 value_t⇄成员⇄存储类别表，跑通。
  - `probe_real_json_value.cpp` —— **修正性探针**：经 `unchecked()` 枚举私有数据成员 → `type_of(m_value)` **间接获得**真实 `basic_json::json_value`，consteval 上下文里枚举 8 成员 + splice 构造/读写全通过（镜像 union 非硬性要求，见 §4.5）。
  - `include/nlohmann/reflection_json.hpp` —— M2/M3 独立 C++26 头（`basic_json_reflection` + `reflection_serializer` + `reflection_cbor_serializer`）。**已升级**：`kStorage`/`kMemberIds` 从**真实 `basic_json::json_value`**（consteval 间接路径）生成，`static_assert` 把本地存储 union 与真实成员表绑定——镜像漂移现在是编译错误而非静默失配（见 §4.5）。
  - `EVALUATION.md` —— **面向社区的评估记录**：concepts-vs-enable_if 与 reflection-vs-macros 的完整测量方法（编译时间/二进制/诊断/代码量）、可复现命令、GCC 16 陷阱、私有成员反射的修正路径。重点分享"评估操作"而非结论。
  - `include/nlohmann/detail/concepts/concepts.hpp` —— C++20 concepts 现代化层（`JSON_HAS_CPP_20` 门控），定义 `floating_point`/`enum_type` 等纯类别概念。
  - `conversions/to_json.hpp`、`from_json.hpp` —— 浮点/枚举重载的 `#ifdef JSON_HAS_CPP_20` 双轨（concepts 约束 vs enable_if），C++11–26 五标准验证一致。
  - `m2_diff.cpp` —— M2 差分测试 vs 主库逐字节等价 + ASan 无泄漏，**PASSED**。
  - `m3_dump.cpp` —— M3 JSON 序列化差分测试（含 ensure_ascii），**PASSED**。
  - `m3_binary.cpp` —— M3 CBOR 差分测试，**PASSED**。
  - `repro_traits.cpp` —— M4 评估：A 类（`is_basic_json`/`is_specialization_of`）反射可行性实证——`is_same_type` 可靠、`template_of` 可识别模板名、但 `template_arguments_of` 受 transient-vector 限制，**PASSED**（结论：能表达、但不值得落地）。
  - `probe_composite_concept.cpp` —— M4/composite 评估：两形态复合 concept（wrap=换皮、expand=手写多条件）与库 `is_compatible_integer_type` 对 9 种类型矩阵判定一致，**PASSED**。结论：复合 concept 技术上可行，但对整型域安全、对容器域（array/object 兼容性）高风险（见 M4_ASSESSMENT §6）。
  - `probe_layered_concepts.cpp` —— 分层概念化实证：层1 原子探测 + 层2 语义概念（带 BasicJsonType + 精确探测目标）与真实 trait 在 15 组合**零漂移**，**PASSED**（见 M4_ASSESSMENT §7）。
  - `probe_draft_drift.cpp` —— 反例实证：**语义复述版**概念（value_type/begin-end/convertible_to json）在 `vector<uint8_t>` binary 维产生 1 处**真实漂移**（会改变序列化行为），证明"精确复刻探测目标"是概念化零漂移的前提，**PASSED（预期 1 漂移）**（见 M4_ASSESSMENT §7）。
  - 全部可复现（需 `g++-16 -std=c++26 -freflection`）：
    ```sh
    cd tests/static-reflection && \
      for f in repro_m1 probe_access probe_union probe_splice m2_table; do \
        g++-16 -std=c++26 -freflection -O2 -o /tmp/$f $f.cpp && /tmp/$f; done
    g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
      -Isingle_include -Iinclude -o m2_diff  m2_diff.cpp  && ./m2_diff
    g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
      -Isingle_include -Iinclude -o m3_dump   m3_dump.cpp   && ./m3_dump
    g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
      -Isingle_include -Iinclude -o m3_binary m3_binary.cpp && ./m3_binary
    g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude \
      -o /tmp/repro_traits repro_traits.cpp && /tmp/repro_traits
    g++-16 -std=c++26 -freflection -O0 -Iinclude \
      -o /tmp/probe_composite probe_composite_concept.cpp && /tmp/probe_composite
    g++-16 -std=c++26 -freflection -O0 -Isingle_include -Iinclude \
      -o /tmp/probe_layered probe_layered_concepts.cpp && /tmp/probe_layered
    g++-16 -std=c++26 -freflection -O0 -Isingle_include -Iinclude \
      -o /tmp/probe_draft probe_draft_drift.cpp && /tmp/probe_draft   # 预期 exit=1（展示 1 处漂移）
    ```
- 编译器行为如有出入，以最小 repro 编译结果为准。
