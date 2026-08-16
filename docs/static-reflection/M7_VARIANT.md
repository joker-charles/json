# M7：顶层 std::variant 支持（oneof 线格式）

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验工作）
> **工具链**：GCC 16.1.0（`g++-16`），`-std=c++26 -freflection`
> **产物**：`refl2::codec` 新增 variant 分支（`reflection_to_json.hpp`）
> + `tests/static-reflection/probe_variant.cpp`
> **状态**：已落地，39 项探针全过（ASan 干净），M5/M6 零漂移双编译不变

## 1. 它是什么

M5 的边界（§8）明说"顶层 variant：排除集拦下，维持原编译错误（variant
集成需 variant_size/variant_alternative，未来扩展）"。本里程碑闭合该边界：
`std::variant<Ts...>` 获得专用 codec 分支，任意位置的 variant（顶层构造、
`j["k"] = v`、结构体成员、容器元素、optional<variant>、variant<variant>）
自动双向序列化，零用户代码。

## 2. 机制

### 2.1 线格式（oneof）

```cpp
json j = std::variant<int, std::string, Point>{Point{1, 2.5}};
// {"index":2,"value":{"x":1,"y":2.5}}
```

- to_json：`{"index": <活动替代项下标>, "value": <替代项序列化结果>}`；
  `std::monostate` 无载荷 → `"value": null`。
- from_json：读 `"index"` 为 `size_t`，经 **index_sequence 折叠**在编译期
  展开的 I 上做运行时分派（`idx == I ? emplace_and_deserialize<B,T,I>()`），
  `emplace<I>()` 后用 `deserialize_one(j.at("value"), std::get<I>(v))`
  递归反序列化（monostate 跳过）；越界 index → type_error 302。

### 2.2 codec 接入点

- **新探针** `is_variant<T>`（`std::variant<Ts...>` 特化匹配）。
- **eligibility**：`to_json_eligible`/`from_json_eligible` 末项由
  `is_reflectable_struct` 扩为 `is_reflectable_struct || is_variant`——variant
  此前因私有成员使 unprivileged 成员计数为 0 而不 eligible，现在显式放行。
- **分派优先级**：nested json 从 priority_tag<6> 提到 <7>，variant 插入
  <6>（optional <5> 之下）。⚠️ **两个分派入口
  （`serialize_one`/`deserialize_one`）必须同步传 `priority_tag<7>`**——
  否则 tag<7> 重载不可达，json 会落到数组分支（实机踩坑：probe_adl_recursion
  的 Date 自定义 from_json 收到对象而非字符串）。
- **成员递归免费获得**：`serialize_member`/`deserialize_member` 走
  `serialize_one`/`deserialize_one` → variant 分支自动生效。
- **循环防御不变**：variant eligible + primary adl_serializer →
  `to_adl_branch_eligible_v` 的末项排除照常触发，variant 不进 adl 分支。

## 3. 与排除集/循环防御的交互（本里程碑的坑）

1. **variant 含 `json` 替代项不支持**（文档化边界）：`std::variant<..., json>`
   的模板实参携带 nlohmann 命名空间 → `in_json_namespace(^^T)` 判定为库内部
   → 排除。这是 M5 循环防御的既定保守行为（ADL 探针会经由 json 替代项看到
   nlohmann::detail 的 catch-all），variant 支持不破坏它。
2. **from_json 要求替代项默认可构造**（`emplace<I>()`）——与结构体路径的
   `T{}` 要求一致；非默认构造替代项在 emplace 处编译错误（非干净
   static_assert，文档化）。
3. **错误路径**：缺 `"index"`/`"value"` → out_of_range 403；负 index /
   非数字 index → get<size_t> 的 302；越界 → 自定义 302。全部与主库异常
   分类一致（本研究库的既有错误通道约定）。

## 4. 验证

`probe_variant.cpp` 39 项：三种替代项（int/string/Point）的线格式与
round-trip；monostate 两态；嵌套位置（结构体成员/vector 元素/map 值/
optional<variant>/variant<variant> 递归）；自定义替代项（结构体/数组/
对象）；错误路径四条；**定制优先**（variant<int, Custom> 内 Custom 走其
adl_serializer 特化——adl 分支在 variant 分支内再次生效）。另更新
`probe_coverage_boundary.cpp`（variant 从负例区移入正例区）并重跑：
probe_adl_recursion / probe_reflection_replace_macros / probe_enum_reflection
全过，M5/M6 零漂移双编译 diff 为空（eligibility 改动对非 variant 类型零影响）。

## 5. 边界

- union 仍不支持（无 index 语义，维持 priority-0 static_assert）。
- variant 的 `json` 替代项不支持（§3.1 循环防御边界）。
- 非默认构造替代项的 from_json 为裸编译错误。
- `valueless_by_exception` 状态：未特殊处理（序列化为其 index，读回时
  emplace 新值——与主库对异常状态的通用处理一致）。
