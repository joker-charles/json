# M5：注解驱动的双向反射序列化器 —— 替换 NLOHMANN_DEFINE_TYPE_* 宏族

> 分支：`feature/static-reflection`
> 对象：`include/nlohmann/reflection_to_json.hpp`（新实验头）+ `detail/conversions/{to,from}_json.hpp` 门控
> 状态：**实现并实机验证**。`probe_reflection_replace_macros.cpp` 25 项全过（含 ASan）；`probe_adl_recursion.cpp` 39 项全过；8 个编译期负例全部按预期失败；零漂移双编译 diff 为空；doctest 套件（cpp11/cpp17/cpp20 双轨目标）全绿。

## 1. 它是什么

在 `g++-16 -std=c++26 -freflection` 下，`nlohmann::detail::{to,from}_json` 重载集各获得一个受约束的 **catch-all 重载**，使**任意可反射结构体**零代码双向序列化：

```cpp
struct Point { double x; double y; };
json j = Point{1.5, -2.0};   // {"x":1.5,"y":-2.0} —— 无宏、无 to_json
auto p = j.get<Point>();     // round-trip
```

成员级注解表达宏族语义（`[[=...]]`，P3394R4 值注解）：

| 宏能力 | 注解替代 |
|---|---|
| 默认（成员名=键、全量） | 无需注解 |
| `_WITH_NAMES`（自定义键名） | `[[=refl2::json_name{"display_key"}]]` |
| 只序列化部分成员 | `[[=refl2::json_ignore{}]]`（默认全量 + 显式排除，与宏"默认排除 + 显式列出"互补） |
| `_WITH_DEFAULT`（缺失键回退 `T{}` 成员） | `[[=refl2::json_default{}]]` |
| 私有成员（INTRUSIVE friend 特权） | `refl2::codec<true>`（unchecked，类型级显式 opt-in）；默认 `codec<false>` 仅公有 |

门控：`#if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)`。非反射构建（C++11/14/17/20/26 不带 `-freflection`）完全不受影响——预处理器层惰性，doctest 套件零回归。

## 2. 机制（整条链路为何"只需一个重载"）

`json j = s` 的既有链路：

```
构造函数 basic_json(CompatibleType&&)      requires is_compatible_type<B,T>
  = is_complete_type<T> && has_to_json<B,T>        (type_traits.hpp)
has_to_json  探测 adl_serializer<T,void>::to_json  (CPO 链)
  -> ::nlohmann::to_json (CPO) -> to_json_fn::operator()
  -> 非限定 to_json(j, val)  ← 普通查找 detail::to_json 重载集 + 实例化时 ADL
```

在 `detail::to_json` 重载集里、`to_json_fn` 定义**之前**声明受约束 catch-all 后，整条链路自动打通：普通结构体 → catch-all 成为唯一候选；手写 `to_json`（类型命名空间，ADL）→ 更特化 + requires 显式排除 → 用户版胜；`adl_serializer<T,void>` 特化 → 构造函数直接调用特化静态成员，**不经过 detail::to_json** → 天然优先。`j.get<T>()` 侧对称（`has_from_json` → `from_json_fn` → `detail::from_json`，catch-all 在 `from_json_fn` 之前声明）。

## 3. 约束工程（catch-all 的 requires）

`to_json_eligible<B,T>` / `from_json_eligible<B,T>` 是现有重载集的**精确负镜像**（逐条复用库自身探针——M4「精确探测目标」纪律，语义复述会漂移）：

- `is_class && !is_union && !is_scalar && !is_array`
- `!is_basic_json`；string：`to_json` 侧用 `is_constructible<string_t,T>`、`from_json` 侧用 `is_assignable<T&,const string_t>` + `is_detected_exact<value_type, value_type_t>`（两侧探针**不同**，各自镜像各自的重载）
- 容器：`!is_compatible_array_type/object_type/binary_type`（to_json）/ `!is_constructible_array_type/object_type`（from_json）
- `!is_constructible_tuple`（结构化绑定结构体）、`!is_specialization_of<std::pair/std::tuple>`（opaque pair/tuple 维持原编译错误，保守）
- `std::filesystem::path`：`is_same`（path 不是类模板，`is_specialization_of` 不适用）
- `!has_user_to_json/from_json`（非循环 ADL 探测，见 §4）
- refl2 `is_reflectable_struct` 形状（`effective_member_count>0 || 不可访问基类`；空/全私有结构体维持原编译错误）

## 4. 循环防护（本里程碑的核心工程）

反射构建下 catch-all 使 `adl_serializer<T,void>::to_json` 对普通结构体**有效**（经 CPO 链）——任何"检测用户定制"的探针若经过 CPO，就会在求值 catch-all 的 requires 时递归。三条防线：

1. **非循环 ADL 探测**（`has_user_to_json/from_json`）：探测放在私有命名空间 `refl2::detail::adl_probe`，非限定 `to_json(j, v)` 的普通查找命中空集、纯 ADL——库的 detail 重载（含 catch-all）对用户类型**不可见**（basic_json 在 `nlohmann`，不在 `nlohmann::detail`），因此探测只找到用户的自由函数，永不重入 catch-all。
2. **`in_json_namespace(^^T)`**（requires 第一项 + 探测偏特化双保险）：`identity_tag<T>` 等库内部类型在 `nlohmann::detail` 命名空间——**ADL 经其自身命名空间可见 detail::from_json**（含 catch-all）→ 探测重入 catch-all requires → 实例化递归（实测：`has_non_default_from_json` 链路把 `identity_tag<Point>` 带进来）。且 `&&` 短路对**模板实例化**无效（嵌套类型成员访问先实例化再求值）——必须在**探测本身**上断：`has_user_{to,from}_json` 对 `in_json_namespace` 类型偏特化为 `false_type`（不探测）。`in_json_namespace` 递归检查**模板实参**（`initializer_list<json_ref<json>>`、`vector<basic_json>` 的实参在 nlohmann 命名空间，ADL 同样可见）。注意 `identifier_of` 对全局命名空间抛 "has_identifier false"——先查 `has_identifier`。
3. **codec 内部 adl 分支排除**：`to_adl_branch_eligible_v` = 原资格 `&& !(to_json_eligible && adl_serializer_is_primary)`。否则普通结构体成员经 codec → `adl_serializer<T,void>` → CPO → catch-all → codec 无限实例化。`adl_serializer_is_primary` 探测 `&adl_serializer<T,void>::template to_json<B,T>` 的存在性——主模板的 to_json 是成员模板（可显式特例化取址），用户特化的常规形态（非模板静态 to_json）不可取址 → 区分"未特化"与"已特化"——特化类型仍走 adl 分支（定制优先），普通结构体走反射分支。

**残余缺口（文档化）**：`adl_serializer<T,void>` 特化声明与主模板同形的**成员模板** to_json 时无法与主模板区分（罕见）。顶层入口（构造函数/`get<T>`）永远优先特化，不受影响。

**from_json 侧特有**：排除集**严禁使用 `is_getable`**（它探测 `j.get<T>()`，经 catch-all 循环）——容器一律用结构探测（`is_array_like`/`is_object_like`/`is_optional`/`is_string_like_from`）。

## 5. 注解层（P3394R4，GCC 16 实机坑位）

- **注解类型必须 structural 且 public**：`std::string`/`std::string_view` 成员被拒（libstdc++ 成员私有 → "does not have structural type"）；`const char*` 成员使 `meta::extract` 抛 "reflect_constant failed"；字符串字面量不能作 NTTP。**`json_name` 用固定大小 `char value[64]`**——structural 且 extractable（超长键名 = 编译错误，即文档化的键长上限）。
- **注解读取 = 纯查询域**：splice 需要实体为常量表达式（consteval 函数参数不是），`template for` 的 range 也要常量——都不可用。用 `annotations_of(member)` 直接下标（transient 规则）+ `meta::remove_cvref`（注解类型是 cv/ref 限定）+ `meta::is_same_type` + `extract<json_name>`（绑定**整个**返回值而非其数组成员——绑定子对象在临时销毁后悬垂，报 "accessing `<anonymous>` outside its lifetime"）。
- **键名以值拷贝传递**：`member_json_key` 返回 `std::array<char,64>`（`string_view` 指向 extract 临时对象 → 非常量表达式且悬垂）；`member_key_v` 是 `inline constexpr std::array<char,64>`，运行时 `static const std::string key = std::string(member_key_v.data())`（沿用 refl2 的一次性初始化模式）。
- `json_ignore` 过滤在**成员索引层**（`effective_member_count`/`effective_member`），反射/反序列化循环只看有效成员。
- `json_default` 的 from_json：`find(key)` 命中 → 递归 deserialize；缺失 → `T{}.member`（要求 T 默认构造，同宏 `_WITH_DEFAULT`）；与位域组合有 4 个 tag 分派重载（无 if-constexpr-with-splices）。

## 6. 与宏的语义差异（有意为之）

| 维度 | 宏 | 反射 |
|---|---|---|
| 成员选择 | 白名单（列出即序列化） | 全量默认，`json_ignore` 排除 |
| 新成员加入 | 需改宏参数 | 自动入 JSON |
| 键名冲突 | WITH_NAMES 静默覆盖（运行时） | `has_duplicate_member_keys` 编译错误（含 json_name 冲突） |
| 私有成员 | INTRUSIVE 隐式 friend | 显式 opt-in（`codec<true>`） |
| 键序 | 宏参数序 | 声明序（JSON 对象键序无语义；std::map 排序输出） |
| 标准面 | C++11–26 | 仅 C++26 + `-freflection`（宏保留为 C++11 路径） |

## 7. 验证

- `probe_reflection_replace_macros.cpp`（25 项）：宏↔注解 4 组对照**逐字节一致**（plain / WITH_NAMES↔json_name / WITH_DEFAULT↔json_default / 部分成员↔json_ignore）、双向入口矩阵（`json j = s`、`j["k"] = s`、`j.get<S>()`、`get_to`、`vector<struct>`/`map<string,struct>` 元素递归、optional、位域、继承）、定制优先（自由 to_json、`adl_serializer<T,void>` 特化——顶层与经 codec 嵌套都胜）、私有成员策略、嵌套定制优先（pitfall fix）。ASan 无泄漏。
- **零漂移**：同一 TU 双编译（`-std=c++26` vs `-freflection`），COMMON 段输出 diff 为空。
- `probe_adl_recursion.cpp`（39 项）与 `probe_coverage_boundary.cpp`：随 codec 迁移更新（`adl_branch_eligible_v` → `to_adl_branch_eligible_v`、`adl_serializer<Celsius,json>` → `<Celsius,void>`——与库 `json_serializer<T,void>` 对齐；"plain struct NOT adl-serializable" 断言更新为"经 catch-all adl-viable 但 adl 分支排除"）。
- 8 个编译期负例（指针成员、私有基类、虚基类、跨层级重名键、空结构体、全私有、union、variant）全部按预期编译失败，消息含可操作指引。
- doctest 套件（`test-serialization_cpp11`、`test-conversions_cpp17`、`test-concepts_dual_cpp20`）全绿；`cxx26-reflect-tests` preset（套件 C++26、无 `-freflection` → 门控关）不受影响。
- 单头 `single_include/nlohmann/json.hpp` 重新 amalgamate 后 C++11 可编译。

## 8. 边界

- 顶层 union/variant：排除集拦下，维持原编译错误（无新 static_assert；variant 集成需 `variant_size`/`variant_alternative`，未来扩展）。
- 结构体内部的指针/变体成员：codec priority-0 static_assert（消息含指引）。
- 非默认构造类型（from_json 的 `T&` 形式）：`has_non_default_from_json` 机制不自动提供（同 refl2 现状）。
- `json_name` 键长上限 63（数组 64）。
- 用户类型声明在 `nlohmann` 命名空间内：被 `in_json_namespace` 误判为库内部（罕见；文档化）。
