# M4D-2：迭代器 + operator[] / find / contains / count / erase(iterator)

> **分支**：`feature/static-reflection`（nlohmann/json 3.12.0 + 实验工作）
> **工具链**：GCC 16.1.0（`g++-16`），`-std=c++26 -freflection`
> **产物**：`reflection_iterator<IsConst>`（`reflection_json.hpp`）
> + `tests/static-reflection/m4d2_iterators.cpp`
> **状态**：已落地，50 项差分检查全过（ASan 干净），全部既有回归绿

## 1. 它是什么

M4D 补全了 tagged-union 的成员函数面，但明确留了边界："迭代器形式
（erase(iterator)/erase(first,last)）需要迭代器里程碑"。本里程碑闭合该
边界：`basic_json_reflection` 获得完整双向迭代器（begin/end/cbegin/cend/
rbegin/rend/crbegin/crend）、`operator[]`（含 null 隐式转型）、
`find`/`contains`/`count`、`erase(iterator)`/`erase(first,last)`——
`iter_impl.hpp` 那 12 处手写 switch 的镜像版本，`basic_json_reflection`
从研究玩具变成可迭代、可修改的镜像库。

## 2. 设计

### 2.1 `reflection_iterator<IsConst>`

镜像 `iter_impl.hpp` 的三模式结构：

| 模式 | 底层 | 运动 |
|---|---|---|
| object | `json::object_t::iterator`（std::map） | 双向 |
| array | `json::array_t::iterator`（std::vector） | 双向 + 偏移 |
| primitive | `ptrdiff_t`（primitive_iterator_t 语义：begin=0, end=1，仅 begin 可解引用） | 0/1 游标 |

- **容器元素是真 `nlohmann::json`**（我们 object/array 容器里存的就是），
  解引用返回 `json&`/`const json&`——与主库完全一致。
- **非容器值不是 json**（union 里是裸标量 / `string_t*` / `binary_t*`），
  primitive 模式解引用把值**物化进迭代器自有的 `mutable json m_scratch`**
  （文档化别名限制：最后一次解引用生效；主库就地解引用真值）。
- 12 处 switch → `mode_for()`（is_* 谓词路由）+ `operator*`/`++`/`--`/
  比较/偏移的类型路由；`erase(iterator)` 分派沿用
  `template for(kValueTInfos)` + 每枚举动作的模式。
- 比较运算保留主库语义：跨容器比较抛错、object 迭代器禁偏移比较、
  `operator[](n)` 仅数组、`key()` 仅对象、null 的 begin==end。

### 2.2 新成员（全部与主库逐条对照）

- **begin/end/cbegin/cend/rbegin/rend/crbegin/crend**：const/非 const 双路径
  （`reflection_iterator<false/true>`），reverse 用 `std::reverse_iterator`。
- **`operator[](size_type)`**：null → 隐式转空数组；越界 → resize 填 null
  （`resize(idx+1)`，同主库）；类型错 → 抛。
- **`operator[](key_type)`**：null → 隐式转空对象；`emplace(key, nullptr)`；
  类型错 → 抛。
- **`find(key)`**：对象 → 定位迭代器；非对象 → end（同主库）。
- **`contains(key)`**：`is_object() && find != end`。
- **`count(key)`**：`is_object() ? object->count(key) : 0`。
- **`erase(iterator pos)`**：容器 → 容器 erase 返回后继迭代器；标量/
  string/binary → destroy() 复位为 null（string/binary 指针被释放，同主库
  的 allocator destroy+deallocate 语义）；null/discarded → 抛；跨容器 → 抛。
- **`erase(first, last)`**：容器区间 erase；primitive 区间必须是
  [begin, end) 否则抛。

## 3. 错误通道

主库迭代器错误抛 `invalid_iterator`（212/213/214/207/208/209 等），类型错
抛 `type_error`；本研究库统一抛 `std::runtime_error`（既有的错误通道约定，
与 M4D 一致）。差分测试对错误路径做 **ok-vs-threw 奇偶对照**（成功 dump
逐字节对照）——精确的 nlohmann 异常分类在本库不可表达，文档化边界（同
M4D 的 tag 对照说明）。

## 4. 验证

`m4d2_iterators.cpp` 50 项：对象/数组/标量/字符串/binary/null 的值迭代、
键值迭代、反向迭代（逐元素 dump 与主库一致）；const 迭代路径；迭代器
算术（+/-、distance、`it[n]`、`n+it`、rbegin 指向末元素）；`operator[]`
成员访问含 null 转型与越界填充；find/contains/count 含非对象语义；
`erase(iterator)`（数组首/中/尾、对象经 find）与 `erase(first,last)` 区间
（dump 奇偶）；标量 erase 复位 null；错误路径四条（null/discarded 上
erase、数组迭代器 key()、对象迭代器偏移、跨容器比较）抛错奇偶。
全部既有差分/探针回归绿（含 M5/M6 零漂移）。

## 5. 边界

- primitive 解引用的 scratch 别名限制（§2.1）——对"迭代即取用"的使用
  无影响，`&*it` 跨迭代器持有时失效。
- 迭代器无 `iterator_category` 之外的随机访问承诺之外的特殊化；
  `std::sort` 等需 `<` 的算法对数组模式可用（object 模式按主库语义拒绝）。
- `operator[]` 的 const 版本未提供（主库同：const 对象用 `at`）。
