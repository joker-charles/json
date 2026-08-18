# M4B-3：CBOR / MessagePack / UBJSON / BJData 读方向

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验工作）
> **工具链**：GCC 16.1.0（`g++-16`），`-std=c++26 -freflection`
> **产物**：`reflection_cbor_parser`、`reflection_msgpack_parser`、
> `reflection_ubjson_parser`（`reflection_json.hpp`）
> + `tests/static-reflection/m4f_binary_readers.cpp`
> **状态**：已落地，671 项差分检查通过（ASan 干净）

## 1. 它是什么

M4B-2 已经用 `kBsonsLoad` 反向表完成了 BSON 读方向。本里程碑把同一模式推广到
其余二进制格式的读方向：

| 格式 | 反向表 | Parser |
|---|---|---|
| CBOR | `kCborLoads` | `reflection_cbor_parser` |
| MessagePack | `kMsgpackLoads` | `reflection_msgpack_parser` |
| UBJSON / BJData | `kUbjsonLoads` | `reflection_ubjson_parser` |

这些表是 256 项 consteval 分类器（`cbor_entry_for` /
`msgpack_entry_for` / `ubjson_entry_for`），把首字节映射到
`{union slot, payload kind}`，slot 仍来自 `slot_index<value_t>`，因此新增
`value_t` 而未接 slot 仍是编译错误。

## 2. 与主库的逐条对应

### 2.1 公共读骨架

- `take` / `read_be` / `take_bytes` / `fail` 与 BSON parser 同构。
- CBOR / MessagePack 使用大端定宽读；UBJSON 按 `use_bjdata` 切换大端/小端。
- 所有 parser 先读到 `nlohmann::json` 临时值，再 `assign_from` 进
  `basic_json_reflection`，与 BSON reader 的接入方式一致。

### 2.2 与库对齐的语义要点（review 阶段修正过的坑）

1. **重复 object key**：主库 SAX DOM parser 通过 `operator[]` 保留**最后一个**
   值；新 parser 不能用 `emplace` 保留第一个值。CBOR / MsgPack / UBJSON 三处
   均已改为 `obj[std::move(key)] = std::move(val)`。
2. **CBOR store tag subtype**：`binary_t::subtype_type` 是 `std::uint64_t`，
   不是 `std::uint8_t`。2/4/8 字节 tag 值必须完整保留，不能截断。
3. **CBOR store tag 后的 indefinite byte string**：主库 `get_cbor_binary`
   接受 `0x5F` indefinite 形式；store 分支已支持。
4. **CBOR 嵌套 indefinite string**：主库允许 indefinite chunk 内再出现
   indefinite chunk；读方向递归处理，保持库行为。
5. **UBJSON `'S'` 后的长度前缀**：该位置**不允许** no-op `'N'`（库的
   `get_ubjson_string(get_char=true)` 直接 `get()`）。不要复用
   `take_ignore_noop`。
6. **UBJSON `'H'` 高精度数字**：必须复用库的 `detail::lexer`，不能用
   `from_chars`/`strtod`。后者会接受空串、`+1`、`0x10` 等库拒绝的文本，并会
   拒绝库接受的尾随空白。
7. **BJData ndarray**：`read_size_value` 支持 `[` 维数向量；`parse_array`
   把 `[$<dtype># [dims...] <data>]` 还原为
   `{_ArrayType_, _ArraySize_, _ArrayData_}`。1D 行向量按库行为退化为普通数组。
8. **长度/计数溢出**：`'L'`/`'M'` 在窄化到 `std::size_t` 前必须做范围检查，
   避免 32 位平台上截断成 0/小值后被错误接受。

## 3. 验证

`tests/static-reflection/m4f_binary_readers.cpp` 覆盖：

- CBOR / MsgPack / UBJSON / BJData 的有效值 round-trip（标量、容器、binary、
  ndarray）；
- 上述 8 类曾经分歧的边界；
- 高精度数字接受/拒绝文本与库逐条一致。

构建：

```sh
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
    -Isingle_include -Iinclude -o m4f_binary_readers \
    tests/static-reflection/m4f_binary_readers.cpp && ./m4f_binary_readers
```
