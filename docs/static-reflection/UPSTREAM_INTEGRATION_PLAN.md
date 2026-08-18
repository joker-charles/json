# 静态反射成果合入 nlohmann/json 主库计划

> **目标**：让静态反射相关成果以“可选扩展、默认零行为变化、C++11 完全兼容、通过上游测试/文档/CI 标准”的方式逐步进入 `develop`。
> **现实预期**：整体静态反射 rewrite 直接合入上游的可能性很低；应拆成小步、可选、可评审的 PR 序列。

## 1. 主库合入的硬性标准

| 标准 | 当前差距 |
|---|---|
| **C++11 contract** | 主库必须 C++11 可编译；当前反射代码是 C++26-only，必须做成完全 gated 的可选扩展，默认不参与编译 |
| **Single-header amalgamation** | 新增/修改 `include/nlohmann/*` 后必须重新生成 `single_include/nlohmann/json.hpp`，不能手改 |
| **测试体系** | 上游要求 `tests/src/unit-*.cpp` doctest、100% coverage、异常用 `CHECK_THROWS_WITH_AS`；当前测试在 `tests/static-reflection/`，不是上游测试布局 |
| **文档** | 需要 `docs/mkdocs/docs` 下的 feature/API 文档，而不是只在 `docs/static-reflection/` |
| **公共 API 稳定** | 不能改签名、异常 ID、默认参数、访问级别；反射路径不能改变现有用户代码的序列化行为 |
| **DCO / PR 规范** | 每个 commit 带 `Signed-off-by`，PR 引用 issue/discussion |
| **编译器矩阵** | 上游 CI 覆盖多编译器；P2996 目前只有 g++-16 较完整，必须作为独立可选 CI job，不能进入默认构建 |

## 2. 最大的三个技术风险

### 2.1 当前实现依赖私有内部结构

`reflection_json.hpp` 用 `access_context::unchecked()` 反射并直接 splice 私有 `basic_json::json_value`。这是**库实现者视角**下的正当用法：P2996 的 `unchecked()` 本来就允许库自身获取私有成员；普通开发者应使用 `unprivileged()`，这是两件不同的事。

`basic_json::json_value` 不是正式契约，但 git 行历史显示该 union 定义自 2020-05-17（`internal_binary_t` → `binary_t` 重命名）以来已超过 6 年未变动。因此可以把它当作**事实上稳定**的内部布局来跟随：即使未来改动，同步更新反射代码即可，预期不会频繁变更。

**上游 PR 时的策略**：
- 保留库内部对 `basic_json::json_value` 的 `unchecked()` 反射，不主动改成公开 API 慢路径；
- 在 PR 中明确说明该内部依赖的稳定性依据（6 年未变 + 库内实现者视角）；
- 如果上游维护者仍不接受这种内部依赖，再退到“最小 `detail` 接口/friend”方案，而不是默认放弃私有反射。

### 2.2 自动 catch-all 会改变用户代码行为

当前反射路径作为 `detail::{to,from}_json` 的 catch-all，会让“任意可反射 struct”自动获得序列化能力。这在上游是**行为变更**，很可能被拒绝。

**对策**：必须做成 opt-in：
- 默认关闭；
- 用户显式定义宏（如 `JSON_USE_REFLECTION`）或显式特化/启用 trait 才生效；
- 开启后仍要与 `NLOHMANN_DEFINE_TYPE_*` 宏路径零漂移。

### 2.3 M4 结论：反射不能替代 type_traits

已有评估（`M4_ASSESSMENT.md`）说明反射不是分类机制的替代品。因此不要提交“用反射重写核心分类”这种 PR。

**对策**：只提交有独立价值的增量：
- C++20 concepts 现代化（若主库改动已符合 C++11 fallback）；
- 可选的反射序列化扩展；
- 反射驱动的枚举/二进制格式扩展（可选）。

## 3. 分阶段计划

### Phase 1：分支“上游化”准备

- 基于最新 `develop` rebase / merge，消除与上游的 drift。
- 精确区分：
  - 主库改动（`include/nlohmann/detail/**`、traits 层）；
  - 实验扩展（`reflection_json.hpp`、`reflection_to_json.hpp`）。
- 跑 `make pretty` + `make amalgamate`，确保格式和 single-header 同步。
- 检查所有 commit 的 DCO。

### Phase 2：先提交安全的主库增量

如果 concepts 现代化已经在主库文件里落地：
- 确认每个改动都有完整 C++11 fallback；
- 在 `tests/src/` 增加 C++11/C++20 双路径单元测试；
- 跑完整 doctest 套件；
- 单独 PR，标题类似：
  `modernize: use C++20 concepts for pure-category to_json overloads (guarded)`

这是最可能先被接受的 PR，因为它不依赖 C++26。

### Phase 3：设计可选反射扩展

- 新增独立头文件，例如 `include/nlohmann/reflection.hpp`，默认不参与主 `json.hpp` 编译，或整体包在 `#ifdef JSON_HAS_CPP_26_REFLECTION` 内。
- 定义 feature macro，例如：
  ```cpp
  #if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection) && defined(JSON_USE_REFLECTION)
  #define JSON_HAS_CPP_26_REFLECTION 1
  #endif
  ```
- 存储访问策略：作为库内部实现，继续使用 `unchecked()` 反射 `basic_json::json_value`；若上游维护者不接受该内部依赖，再退到受控 `detail` 接口/friend。
- 默认行为不变：没有宏时，编译结果与现在的主库完全一致。

### Phase 4：按上游质量补齐测试与文档

- 把 `tests/static-reflection/m4f_binary_readers.cpp` 等有价值的差分测试迁移/补充为：
  - 主测试套件中的可选测试（feature-gated）；
  - 上游风格 doctest；
  - compile-fail 测试（无反射编译器、未启用宏等情况）。
- 增加 CI job：
  - g++-16 `-std=c++26 -freflection` 跑反射扩展测试；
  - 常规 C++11/14/17/20 job 确认主库无回归。
- 文档：
  - `docs/mkdocs/docs/features/reflection.md`；
  - API 页面；
  - 性能/编译时间数据（可选扩展的代价）。

### Phase 5：拆分 PR 序列

不要一次提交大爆炸。建议顺序：

1. **PR 1**：C++20 concepts 现代化（gated，C++11 fallback）。
2. **PR 2**：可选反射扩展头文件 + 宏 + 单元测试 + 文档（不接入默认路径）。
3. **PR 3**：如果维护者接受方向，再提“默认关闭的反射 catch-all 接入 `to_json/from_json`”的增量。
4. **PR 4**：二进制格式读方向等扩展，作为独立可选特性。

每个 PR 都要：
- 引用 upstream discussion/issue；
- 有 DCO；
- 有 Changelog 条目；
- 有覆盖率报告；
- 有 `make amalgamate` 后的 single-header diff。

## 4. 上游沟通策略

在写大量代码之前，先开一个 discussion 问维护者：

> “Would an opt-in, C++26-only reflection extension behind a feature macro be acceptable for nlohmann/json, given it does not change default behavior and keeps C++11 support?”

这非常重要。如果方向被否，就不要继续往主库方向投入；如果被接受，按 Phase 3–5 推进。

## 5. 现实预期

- **最可能合入**：concepts 现代化、可选扩展头文件、文档/测试基础设施。
- **不太可能合入**：用反射重写核心 tagged-union、默认自动 catch-all、未经维护者确认就把私有布局当公共契约的代码。
- **建议**：把“合入主库”当作长期目标，当前先把实验分支整理成“可评审的上游质量 PR 候选”，即使最终不合入，也能极大提高分支的可维护性。
