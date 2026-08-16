# docs/static-reflection — 文档索引

本目录是 `feature/static-reflection` 分支的实验文档：用 **P2996 静态反射**重写
nlohmann/json 的 tagged-union 核心，并用 **C++20 concepts** 现代化序列化层。
文档按「总览 → 里程碑 → 参考」三层组织，本页是导航入口。

> 工具链硬约束：反射代码只认 `g++-16 -std=c++26 -freflection`（详见
> `BUILD_RECIPES.md` 与 `VERIFIED_FACTS.md`）。

## 读什么、何时读

| 你想做 | 读 |
|---|---|
| 快速知道「结论 + 实测数据」 | [`EVALUATION.zh-CN.md`](EVALUATION.zh-CN.md)（中文）或 [`EVALUATION.md`](EVALUATION.md)（英文） |
| 理解「为什么这么设计」 | [`FEASIBILITY.md`](FEASIBILITY.md) |
| 查某个里程碑的实现 / 边界 / 坑 | 下方「里程碑设计」对应文档 |
| 查已验证的编译器 / 类型事实（别重新踩坑） | [`VERIFIED_FACTS.md`](VERIFIED_FACTS.md) |
| 配置、构建、复现、测量 | [`BUILD_RECIPES.md`](BUILD_RECIPES.md) |

## 文档分类

### 一、总览与评估

- **`EVALUATION.md` / `EVALUATION.zh-CN.md`** — 主评估报告（1073 / 894 行，带目录）。
  回答三个评估问题：concepts vs enable_if、reflection vs macros、方法论 takeaways；
  含全部实测数据（编译时间 / 二进制大小 / 运行时吞吐 / 诊断质量）与复现步骤。
- **`FEASIBILITY.md`** — 可行性分析 + 设计方案（283 行）。反射的天然靶点、
  收益排序、C++11 契约冲突、GCC 16 已验证反射模式清单、里程碑落地顺序。

### 二、里程碑设计（按时间顺序）

| 里程碑 | 文档 | 内容 |
|---|---|---|
| M0–M3 | `FEASIBILITY.md` + `VERIFIED_FACTS.md` | 反射核心：`value_t` 表、tagged-union、存储类别（无独立文档） |
| M4 | `M4_ASSESSMENT.md` | `type_traits` 反射可替代性**结论**（反射不替换 detection idiom）+ concepts 现代化边界 |
| M4B / M4B-2 | （无独立文档，见 `VERIFIED_FACTS.md`） | 二进制格式写 / 读（msgpack·ubjson·bson·cbor） |
| M4D | `M4D_API_SURFACE.md` | tagged-union API 面：`type_name`/`at`/`erase`/`clear`/`swap`/比较 + `value_t` 表 |
| M4D-2 | `M4D2_ITERATORS.md` | 迭代器 + `operator[]`/`find`/`contains`/`count`/`erase` |
| M4E | `M4E_UBJSON_OPT.md` | UBJSON 优化模式 + BJData 写方向 |
| M5 / M6 | `M5_REFLECTION_TO_JSON.md` | refl2 双向反射序列化器（替换 `NLOHMANN_DEFINE_TYPE_*`）+ `json_name`/`json_ignore`/`json_default` 注解 + 枚举字符串映射 |
| M7 | `M7_VARIANT.md` | 顶层 `std::variant` 支持（oneof 线格式） |

> 编号说明：M0–M7 是反射主线；M4B / M4D / M4E 是 M4 之后的三条并行支线
> （二进制格式 / API 面 / UBJSON）。M4B 与 M4B-2 没有独立设计文档，其事实与
> 验证记录在 `VERIFIED_FACTS.md` 和 `AGENTS.md` §1。

### 三、参考与复现

- **`VERIFIED_FACTS.md`** — 本工具链上已实证的编译器 / 类型事实（357 行）。
  写任何反射 / concepts / 序列化代码前先读，避免重新踩坑。
- **`BUILD_RECIPES.md`** — 工具链、各探针编译命令、单头 amalgamate、仓库测试套件（122 行）。
- **`data/`** — 测量快照（`measure_results_*.txt` 编译基准、`runtime_measure_*.txt` 运行时基准）。

## 推荐阅读路径

1. **新人入门**（想理解这个分支在做什么）：
   `EVALUATION.zh-CN.md`（结论 + 方法）→ `FEASIBILITY.md`（设计）→ `M5_REFLECTION_TO_JSON.md`（核心实现）。
2. **查证事实**（写代码前）：
   `VERIFIED_FACTS.md`（编译器 / 类型事实）→ `BUILD_RECIPES.md`（工具链 / 构建）。
3. **复现测量**：
   `BUILD_RECIPES.md` + `EVALUATION.md` §4「Reproduce」。
4. **审阅某个里程碑**：
   直接看对应 `M*` 文档的「验证」与「边界」两节。

---

*测试与基准源码在仓库 `tests/static-reflection/`（探针 `probe_*`、差分 `m*_diff` /
`m4*`、负向编译 `compile_fail/`、基准 `bench_*`）。*
