# M4E：UBJSON 优化模式 + BJData 写方向

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验工作）
> **工具链**：GCC 16.1.0（`g++-16`），`-std=c++26 -freflection`
> **产物**：`reflection_ubjson_optimized_serializer`（`reflection_json.hpp`）
> + `tests/static-reflection/m4e_ubjson_opt.cpp`
> **状态**：已落地，63 用例 × 4 模式（UBJSON + BJData draft2/draft3）逐字节
> 差分通过（630 项检查，ASan 干净）

## 1. 它是什么

M4B 的 `reflection_ubjson_serializer` 只覆盖 UBJSON 的 no-optimization 模式
（`use_count=false, use_type=false`，即库 `to_ubjson` 的默认路径）。本里程碑
补全写方向的剩余缺口（FEASIBILITY §2.3 明列）：

1. **UBJSON 优化模式**：`'#'` 计数前缀（use_count）、`'$'` 类型前缀
   （use_type，**要求 use_count**，与库的 `JSON_ASSERT(use_count)` 一致）；
2. **BJData**（use_bjdata）：小端数值/长度、`'u'`/`'m'`/`'M'` 宽度阶梯、
   `'$'` 类型优化的排除表（bjdx）、draft2/draft3（draft3 的 binary 用 `'B'`
   标记）、JData ndarray 特判。

## 2. 与主库的逐条对应（binary_writer.hpp write_ubjson）

### 2.1 公共骨架

- **add_prefix**：顶层恒为 true（本树 `to_ubjson`/`to_bjdata` 静态入口的
  `write_ubjson(j, use_size, use_type)` 走 add_prefix 默认值 true，不暴露该
  参数）；递归元素按 `prefix_required` 传递（容器优化成功 → false）。
- **分派**：`template for(kValueTInfos)` + 每枚举 NTTP 动作 `write_one<V>`，
  字节码表 `kUbjsonCodes` 仍是主码单源；`'#'`/`'$'`、宽度阶梯、逐元素前缀
  在动作内选择——与 no-opt 写方向完全同构的分工。

### 2.2 数值阶梯（write_number_with_ubjson_prefix）

| 阶梯 | 无符号 | 有符号 |
|---|---|---|
| int8 | `'i'` ≤ int8_max | int8 范围 → `'i'` |
| uint8 | `'U'` ≤ uint8_max | `0..uint8_max` → `'U'` |
| int16 | `'I'` ≤ int16_max | int16 范围 → `'I'` |
| uint16 | **`'u'`（BJData 独有）** | `0..uint16_max` → `'u'`（BJData） |
| int32 | `'l'` ≤ int32_max | int32 范围 → `'l'` |
| uint32 | **`'m'`（BJData 独有）** | `0..uint32_max` → `'m'`（BJData） |
| int64 | `'L'` ≤ int64_max | int64 范围 → `'L'` |
| uint64 | **`'M'`（BJData 独有）**；非 BJData 超出 int64_max → `'H'` 高精度十进制 | — |

浮点：`number_float_t` 是 double → 恒 `'D'` + float64（不压 float32，已验证
事实）。`'H'` 路径：`'H'` + 长度（走无符号阶梯）+ 十进制数字
（`json(n).dump()` ≡ `std::to_chars` 整数字符串）。

**端序（本里程碑最易错的一点）**：BJData 模式**所有**数值与长度前缀都是
**小端**——库的 `write_number(n, OutputIsLittleEndian=use_bjdata)` 对整型、
无符号、浮点一视同仁（不是"无符号小端、有符号大端"）。UBJSON 全大端。

### 2.3 容器优化（use_type ⇒ use_count）

- **数组**：`first_prefix = ubjson_prefix(front)`，`same_prefix` 检查
  `begin()+1..end`（首个元素假定同）；对象检查**全部值**的 prefix。
- **排除表**：`use_bjdata` 且 first_prefix ∈ `{'[','{','S','H','T','F','N','Z'}`
  → 不做 `'$'` 优化（这些标记在 BJData 里语义歧义）；普通 UBJSON **无此
  限制**（`['$S#i2"a""b"]` 合法）。
- 命中：`'$'` + prefix，然后 `'#'` + 计数，元素不带前缀；未命中：仅
  `'#'` + 计数，元素带前缀；`!use_count`：`']'`/`'}'` 收尾。
- 计数与键长都走**无符号阶梯**（小端 if BJData）。

### 2.4 binary

`'['` +（use_type：`'$'` +（draft3 ? `'B'` : `'U'`）；draft2 空 binary **跳过
`'$'`**）+（use_count：`'#'` + 长度）+（use_type：原始字节 | 否则逐字节
`'U'`/`'B'` + 字节）+（!use_count：`']'`）。

### 2.5 JData ndarray（仅 BJData）

对象恰好含 `{_ArrayType_, _ArraySize_, _ArrayData_}` 三个键时：
`'[' '$' <dtype> '#' <_ArraySize_ 整体递归编码> <_ArrayData_ 紧凑元素>`。
dtype 映射 12 种（uint8..int64/single/double/char/byte → 'U'/'i'/'u'/'I'/
'm'/'l'/'M'/'L'/'d'/'D'/'C'/'B'）。**回退为普通对象**的条件（逐条复刻）：
dtype 不在表内、维度非非负整数、维度溢出 size_t、维度乘积溢出、元素总数
≠ 尺寸乘积、元素种类与 dtype 不符（浮点 dtype 要 float，整型 dtype 要
integer——注意 `get<>` 同时接受 signed/unsigned 存储形态）。紧凑元素一律
小端（`write_number(..., true)` 硬编码）。

## 3. 验证

`tests/static-reflection/m4e_ubjson_opt.cpp`：63 个用例（标量宽度全边界
int8..uint64 含 BJData 'u'/'m'/'M' 命中、±0.0/1e100、空/长字符串、同质与
异质容器、排除表命中与未命中、256 元素计数跨界、binary 空/非空/带 subtype、
6 组合法 ndarray + 4 组非法回退、discarded）×(UBJSON + BJData draft2 +
draft3) × {uc,ut} 合法组合 = 630 项逐字节对照，全部与
`json::to_ubjson`/`json::to_bjdata` 一致；另含 (F,F) 与既有
`reflection_ubjson_serializer` 的三方互证。ASan 干净。

## 4. 边界

- 读方向（from_ubjson/from_bjdata）仍未实现（本轮既定范围）。
- BJData draft3 只复刻了 binary `'B'` 标记差异（主库 draft3 的全部差异）。
- `'$'` 优化的 same_prefix 检查对空容器跳过（`!empty` 守卫，同主库）。
