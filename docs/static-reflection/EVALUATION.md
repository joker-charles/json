# Reflection vs Macros vs Concepts: Measured Evaluation Notes

> **Branch**: `feature/static-reflection` (nlohmann/json 3.12.0 + experimental work)
> **Toolchain**: GCC 16.1.0 (`g++-16`), flags `-std=c++26 -freflection` (reflection),
> `-std=c++20` (concepts), `-std=c++11` (baseline enable_if)
> **Status**: re-measured from scratch on 2026-08-15 (this machine, 12th Gen
> i5-12600KF, no ccache, g++-16 16.1.0-2ubuntu1); raw runs in the measurement
> log (§5). Every number below was re-derived in that session — the previous
> version of this document carried numbers that did **not** survive re-measurement;
> §5 lists exactly which claims were corrected.

This document records *how* we evaluated two modernization directions for a
header-only template library (nlohmann/json) — (A) replacing hand-written
macros/SFINAE with C++20 concepts, and (B) replacing user-facing macros with
P2996 static reflection. The goal is to share the **evaluation operations**
(measurement setups, comparison axes, pitfalls) so others can reproduce or
challenge the numbers, not just the conclusions.

---

## 0. The three evaluation questions

For each direction we asked the same four questions, and answered them with
measurable evidence rather than opinion:

| # | Question | Metric |
|---|---|---|
| Q1 | Does it save code? | user-side lines per type, marginal cost, break-even point |
| Q2 | Compile-time cost? | wall-clock / peak RSS of the SAME TU at the SAME standard, before vs after |
| Q3 | Binary / generated-code bloat? | executable size, `size` text/data, section attribution |
| Q4 | Diagnostics better or worse? | error-line count, first error, whether failure is silent |

Plus a correctness question that turned out to be the most important:

| Q5 | Do the non-happy paths work? | private members, nested types, ADL customization |

---

## 1. Evaluation operation A: concepts vs enable_if (dual path)

The library keeps a C++11 enable_if path and a C++20 concepts path for the same
overloads (`#ifdef JSON_HAS_CPP_20`). The whole point of the dual path is that
**both must select the same overloads**. The evaluation therefore compares the
same translation unit compiled under different standards.

### 1.1 Setup — the baseline matters (before vs after, same standard AND flags)

The modernization target is **C++26 + static reflection** (the branch's goal),
so the headline comparison must be before (develop baseline `cdf52ae9`, pure
enable_if) vs after (this branch) at `-std=c++26 -freflection`. The dual path
lives in the SAME headers (`#ifdef JSON_HAS_CPP_20`), so the full matrix also
isolates the concepts slice (c++20), the standard upgrade (c++20→c++26), and
the `-freflection` flag cost — none of which should be conflated.

Measurement protocol: every configuration was compiled **3 times**, and the
minimum wall time is reported (template-heavy compiles are noisy; a single run
is not a number). Sizes are decimal KB (1 KB = 1000 B), matching `ls`/`stat`.

```sh
# baseline: git worktree add /tmp/json-baseline develop   (cdf52ae9)
FLAGS="-Wno-deprecated -Wno-float-equal -Wno-deprecated-declarations
       -DDOCTEST_CONFIG_SUPER_FAST_ASSERTS -DJSON_TEST_KEEP_MACROS
       -DJSON_TEST_USING_MULTIPLE_HEADERS=1 -Itests/thirdparty/doctest
       -Itests/thirdparty/fifo_map"

# HEADLINE: full modernization, before vs after at c++26 + freflection
/usr/bin/time -f "wall=%e s maxrss=%M KB" \
  g++-16 -O0 -std=c++26 -freflection $FLAGS -I/tmp/json-baseline/include \
  -c tests/src/unit-serialization.cpp -o /tmp/before.o
/usr/bin/time -f "wall=%e s maxrss=%M KB" \
  g++-16 -O0 -std=c++26 -freflection $FLAGS -Iinclude \
  -c tests/src/unit-serialization.cpp -o /tmp/after.o
```

**Pitfall (we made this mistake)**: comparing c++11 vs c++20 reports the
cost of the *standard upgrade* as if it were the concepts cost; and pinning
everything to c++20 hides the C++26 + reflection target. The fair comparison
holds both the standard AND the flags fixed.

### 1.2 Results (unit-serialization.cpp, one TU)

**Headline: full modernization (before vs after @ c++26 -freflection, min of 3):**

| | before (develop) | after (branch) | delta |
|---|---|---|---|
| wall @ -O0 | 3.55 s | 3.59 s | **+1.1% (noise)** |
| .o @ -O0 | 2,612 KB | 2,628 KB | +0.6% |
| wall @ -O2 | 6.36 s | 6.35 s | **−0.2% (noise)** |
| .o @ -O2 | 700 KB | 700 KB | ~0% |

**The full modernization (concepts + reflection-ready) costs ~zero** at the
target standard: −0.2% to +1.1% wall, ≤0.6% code size. Both instantiate the
same `external_constructor<...>::construct`; concepts only pick overloads and
the reflection header is not even included by this TU.

**Cost decomposition (isolate each factor, -O0, min of 3):**

| configuration | wall | factor isolated |
|---|---|---|
| before @ c++20 | 3.14 s | baseline |
| before @ c++26 | 3.44 s | **+9.6%** standard upgrade |
| before @ c++26 -freflection | 3.55 s | **+3.2%** reflection flag |
| after @ c++20 | 3.15 s | concepts slice **≈ 0%** |
| after @ c++26 -freflection | 3.59 s | full modernization |

**Takeaways from the decomposition**:
- concepts slice at c++20: **≈ 0%** — the concepts dual path is free (the old
  report's −5% did not reproduce; see §5)
- standard upgrade c++20→c++26: **+9.6%** (old report: +3%)
- `-freflection` flag (even with no reflection code in the TU): **+3.2%**
  (old report: +7–8%)
- full modernization at target: **≈ 0–1%** (the factors roughly cancel)

Cross-standard reference (all min wall; .o sizes at -O0 in parens):

| path | -O0 wall | -O2 wall | .o @ -O0 |
|---|---|---|---|
| c++11 (enable_if) | 2.28 s | 4.89 s | 2,555 KB |
| c++20 (concepts)  | 3.15 s | 5.44 s | 2,595 KB |
| c++26 +refl (branch) | 3.59 s | 6.35 s | 2,628 KB |

Runtime (fresh bench TU: 200 iterations over a 200-object complex JSON, -O2,
before vs after at c++26, 6 interleaved runs each; see §5 for the source):

| | before | after | verdict |
|---|---|---|---|
| serialize | 0.064–0.068 ms/op | 0.069–0.074 ms/op | within build-layout noise (±5–8%: rebuilding the SAME source twice moves this by as much) |
| parse | 0.411–0.433 ms/op | 0.406–0.422 ms/op | no regression (overlapping ranges) |

`dump()` is code-identical before/after (the branch changes the lexer and the
concepts dual path, not the serializer), so the serialize "gap" is a code-layout
artifact: three builds of the *identical* source spanned 13.04–14.14 ms/200it,
which is the same size as the before/after delta. Parse is at worst unchanged
and at best slightly faster (lexer `from_chars`).

### 1.3 Diagnostic evaluation (Q4)

The interesting case is *failure*: a type with no `to_json`. Target standard
(c++26 -freflection), before vs after:

```sh
cat > err.cpp <<'EOF'
#include <nlohmann/json.hpp>
struct not_serializable { int x; };
int main() { nlohmann::json j; not_serializable ns; nlohmann::to_json(j, ns); }
EOF
g++-16 -std=c++26 -freflection -I/tmp/json-baseline/include -fsyntax-only err.cpp 2> before.txt
g++-16 -std=c++26 -freflection -Iinclude -fsyntax-only err.cpp 2> after.txt
wc -l before.txt after.txt   # 238 vs 376  (reproduces exactly)
```

| | before (enable_if) | after (concepts) |
|---|---|---|
| error output | 238 lines / 27,987 B | 376 lines / 42,478 B (+58% lines, +52% bytes) |
| first error | `err.cpp:3:70: error: no match for call to '(const ...::to_json_fn)(json&, not_serializable&)'` | identical first error |
| why | enable_if failures are opaque (`enable_if<false>` never says *why*) | 7× `required for the satisfaction of ...` chains ending in `the required expression 'std::begin(t)' is invalid` |

**Interpretation**: concepts output is ~1.6x longer but each failure names the
*reason* (which probe failed, at which concept line). Both paths share the same
first error line (call site + `no match for call to 'to_json'`), so the "what
went wrong" anchor is identical; concepts add the "why". These counts **reproduce
the previous report exactly** (238 vs 376).

**Pitfall**: comparing errors via `j = ns` (assignment) is useless — it fails in
`basic_json`'s own ctor SFINAE before reaching `to_json`, identically in both
standards. You must call `nlohmann::to_json(j, ns)` directly to exercise the
overload set you changed.

### 1.4 Non-happy path: the alt_string regression (Q5)

The concepts layer carries `BasicJsonType` in `string_like`/`object_like`/
`array_like`. Evaluation with the *default* `std::string` string_t passes even
when the concepts are declared with the wrong parameter order, because the
mis-binding happens to agree. It took a **custom string_t TU**
(`tests/src/unit-alt-string.cpp`) to expose the real bug:

- concepts declared `template<typename B, typename T>` (BasicJsonType first)
- constrained placeholder `concepts::string_like<BasicJsonType> S` — GCC binds
  `S` to the **first** parameter and `<BasicJsonType>` to the **second**
- ⇒ for a custom `alt_string` string_t, the user type lands in `B`, and
  `typename B::string_t` becomes a hard error (`no type named 'string_t' in
  'class alt_string'`)
- fix: declare `template<typename T, typename B>` (candidate first); verified by
  minimal repro (see below)

**Evaluation lesson**: a differential probe over the default type is *not*
enough; you need at least one non-default template-parameter TU in the matrix.
Minimal repro (re-verified on g++-16, `-std=c++20` — right order compiles, wrong
order fails with `no type named 'string_t' in 'struct alt_string'`):

```cpp
template<typename T, typename B>
concept StringLikeRight = std::is_constructible_v<typename B::string_t, T>;
template<typename B, StringLikeRight<B> S> void f(const S&);   // <T, B>: OK
// declared <B, T> instead -> B binds to S !!  (hard error on alt_string)
```

See AGENTS.md §3 "Partial-application concepts carry BasicJsonType LAST".

---

## 2. Evaluation operation B: reflection vs macros

Question: "does P2996 reflection let us delete `NLOHMANN_DEFINE_TYPE_INTRUSIVE`?"

### 2.1 Setup

Benchmark probe `tests/static-reflection/bench_macro_vs_reflection.cpp`:
N person-like structs `{ name (std::string), age (int), height (double),
tags (std::vector<std::string>), active (bool) }`, generated by an X-macro.
The same TU compiles in **three modes** selected by the `-DBENCH_*` flags:

- **macro version** (`-DBENCH_N` only): `NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_i,
  name, ...)` — one macro call inside each struct
- **refl v1** (`-DBENCH_REFLECTION`): the naive generic `to_json`/`from_json`
  over `nonstatic_data_members_of`; user types get **zero** declarations
- **refl2 v2** (`-DBENCH_ADL_REFLECTION`): the ADL-aware recursive serializer
  (new in this revision) — explicit `adl_serializer` calls, container element
  recursion, per-member reflection recursion, `unprivileged()` default

`-DBENCH_N` (default 50) controls how many of the 100 defined types are
instantiated in `main()` (unused types generate no code). All three modes
compile with the SAME flags (`-std=c++26 -freflection`), and `main()` keeps
the serializers observable (their output feeds a printed counter), so `-O2`
cannot eliminate the generated code — a deliberate design choice, see the
caveat below. All three modes print the same counter (identical JSON), so the
§2.2 numbers are pure serializer cost.

Reflection serializer **v1** (the one-time cost; the `namespace refl` block in the
probe is **24 lines** plus 1 `#include <meta>` = 25 lines):

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

**Pitfalls hit while building v1 (all verified GCC 16 behaviors; unchanged
from the previous revision)**:
- `std::meta::` must be qualified; `std::define_static_array` (not
  `std::meta::define_static_array`)
- `nonstatic_data_members_of` returns a **transient vector**: subscript/size
  directly on the call, never bind to a local `constexpr`
- `template for` needs the range inline or namespace-scope constexpr
- recursive serialization of nested class members does NOT fall into nlohmann's
  ADL overload set (`detail::to_json` is not a bare function); scalars must go
  through assignment / `.template get<M>()` — **v1 cannot serialize a nested
  plain struct or a container of plain structs at all**
- `if constexpr` does NOT discard the false branch ⇒ tag-dispatch required for
  class-vs-scalar recursion

Reflection serializer **v2 ("refl2")** — the ADL-aware recursive codec added
in this revision (mode `-DBENCH_ADL_REFLECTION`; reference implementation
exercised by `tests/static-reflection/probe_adl_recursion.cpp`, see §2.5).
It implements the recursive dispatch inside the serializer itself:

```
serialize_one(j, v), highest priority first:
  5  nested basic_json value          -> j = v               (native nesting)
  4  adl branch (strings + non-container adl-able types)     -> adl_serializer<T, B>
  3  array-like container             -> per-element recursion
  2  object-like container (map-like) -> per-value recursion (string keys)
  1  reflectable struct               -> per-member reflection recursion
  0  anything else                    -> static_assert diagnostic
```

- The adl branch calls `nlohmann::adl_serializer<T, B>` explicitly — the
  public extension point — so **customization wins over reflection**: a
  nested member with a user `to_json` is caught by the extension machinery
  (this is the fix for v1's recursion pitfall), and a nested plain struct
  falls through to the reflection branch and recurses.
- Detection traits (`is_adl_serializable`, `is_reflectable_struct`, ...) are
  implemented here on the public API only — nothing from the library's
  `detail` internals. Dispatch is overload ranking on `priority_tag` (the
  same idiom nlohmann uses internally); there is no
  `if constexpr`-with-splices anywhere.
- Access policy: reflection uses `access_context::unprivileged()` by default
  (`codec<false>`) — private members are neither serialized nor parsed;
  `codec<true>` switches to `unchecked()` for library-internal use (the
  two-layer policy from §2.3, demonstrated in §2.5).
- The whole `namespace refl2` block is **311 lines** plus the (shared)
  `#include <meta>` = 312 lines one-time cost.

Two new pitfalls were hit and fixed while building v2 (reproduced by the
probe):
- **The nested-json string_like trap**: `basic_json` has an explicit template
  conversion operator (delegating to `get<ValueType>()`), so
  `std::is_constructible_v<std::string, json>` is TRUE. nlohmann's own
  `string_like` concept therefore matches a `json` VALUE and
  `adl_serializer<json, json>::to_json` resolves to the string path, which
  calls `get<std::string>()` and throws on any non-string value. Fix: the
  nested-json branch (priority 5) precedes the adl branch.
- **from_json over-acceptance of containers**: nlohmann's from_json
  detection treats `map<string, PlainStruct>` as adl-usable (the tuple
  machinery makes `get<pair<const string, PlainStruct>>` look viable) but
  breaks on instantiation. Fix: array/object-like containers are excluded
  from the adl branch (`adl_branch_eligible`) and always go through the
  codec's own element recursion — behavior-identical for what nlohmann
  handles, and it additionally covers containers of plain reflected structs.

### 2.2 Results

Re-measured in one session (three modes, same TU, same `-std=c++26
-freflection`, min of 3; raw runs in §5). All three modes print the same
counter — the JSON output is identical — so the numbers are pure serializer
cost, not behavior drift.

**Q1 code saved** (per-type user lines: macro = struct + macro call = 2/type;
reflection = struct only = 1/type; one-time cost: v1 = 25 lines, v2 = 312):

| types | macro | refl v1 | refl2 v2 |
|---|---|---|---|
| 1 | 2 | 26 | 313 |
| 20 | 40 | 45 | 332 |
| **25** | 50 | 50 | 337 |
| 50 | 100 | 75 | 362 |
| 100 | 200 | 125 | 412 |
| **312** | 624 | 337 | 624 |

Both reflection versions save **1 line per type**; the naive v1 needs a
**25-line** one-time serializer (break-even ≈ 25 types) but only works for
flat structs, while the complete v2 (recursion + ADL + private-member policy)
needs a **312-line** one-time serializer (break-even ≈ 312 types) and handles
nested structs, containers of plain structs, user customization and
`unprivileged()` access control — which v1 cannot do at all.

**Q2 compile time** (same TU, same flags, min of 3):

| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 2.23 s | 2.34 s | 2.27 s | 3.12 s | 3.31 s | 3.12 s |
| 20 | 2.31 s | 2.52 s | 2.45 s | 3.23 s | 3.81 s | 3.31 s |
| 28 | 2.40 s | 2.63 s | 2.52 s | 3.41 s | 4.11 s | 3.45 s |
| 50 | 2.63 s | 2.87 s | 2.78 s | 3.88 s | 4.99 s | 4.00 s |
| 100 | 3.06 s | 3.51 s | 3.35 s | 4.99 s | 6.46 s | 4.92 s |

v2 is **cheaper to compile than v1** at every measured point (−2.8…−4.6% at
-O0, −6…−24% at -O2) and close to the macro baseline (+1.8…+9.5% at -O0;
−1.4…+3.1% at -O2). The dispatch machinery is resolved at compile time and
adds less per-type instantiation work than v1's per-member `j[...] = v.[:m:]`
+ `std::string(identifier_of(m))` codegen.

**Q3 binary** (executable bytes / `size` text, per N):

| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 393,656 / 169,117 | 484,512 / 198,990 | 395,632 / 169,901 | 113,384 / 80,047 | 119,888 / 85,148 | 113,384 / 80,047 |
| 20 | 445,936 / 202,452 | 555,608 / 260,004 | 501,944 / 217,336 | 131,088 / 94,096 | 174,456 / 129,198 | 131,088 / 94,128 |
| 28 | 470,560 / 216,484 | 584,264 / 285,692 | 546,544 / 237,304 | 145,120 / 106,145 | 194,248 / 149,898 | 145,120 / 106,177 |
| 50 | 530,088 / 255,090 | 669,208 / 356,342 | 666,120 / 292,223 | 182,608 / 138,230 | 265,248 / 207,818 | 182,608 / 138,262 |
| 100 | 667,600 / 342,916 | 850,360 / 517,028 | 939,752 / 417,157 | 266,640 / 208,829 | 421,720 / 342,878 | 266,640 / 208,861 |

**The headline**: at **-O2, v2 is byte-identical to the macro version** — the
executable is exactly equal at every N (and `text` within 32 bytes; `nm` 288
symbols at N=50, identical to macro, vs 300 for v1). The v2 dispatch
(priority_tag ranking, adl_serializer indirection, reflection recursion)
fully collapses under optimization: both paths converge on the same
`adl_serializer`/`external_constructor` code the macro generates directly.
v1 does NOT converge (its per-member string-key conversion + assignment
survives optimization: +6…+58% exe at -O2) — so v2 is **smaller than v1 at
-O2 by −5…−37%**.

At **-O0**, v2 sits between macro and v1: near-macro at N=1 (+0.5%), growing
to +40.8% exe at N=100 (v1: +27.4%). The -O0 overhead is the un-collapsed
dispatch: `nm` at N=50 counts 1,871 symbols for v2 vs 1,621 (v1) / 1,471
(macro). The old §2.2 lesson still stands — always state the optimization
level and keep the generated code observable — but the new finding is that
the -O0 bloat of v2 is *mostly optimization artifacts*: it evaporates at -O2
to exactly zero.

**Q4 diagnostics** — verified with a deliberate bad type (`std::mutex`
member, which is neither adl-able nor reflectable):

| scenario | macro | refl v1 / refl2 v2 |
|---|---|---|
| member unserializable, listed in the macro | compile error naming `std::mutex` at the macro expansion | compile error naming `std::mutex` at the serializer line |
| member not listed in the macro | **silent** lossy round-trip (member dropped, zero diagnostics) | impossible: reflection sees all public members — a public unserializable member is a compile error |
| v2-only: unserializable *type* (no to_json, not reflectable) | — | v2 emits **one** static_assert with an actionable message ("define a to_json or specialize nlohmann::adl_serializer"): 11 error lines vs 238 (enable_if) / 376 (concepts) for the same failure via the library's own overload set |

Interpretation (unchanged from the previous revision): both macro and
reflection fail at compile time for a *listed* unserializable member; the
macro hazard is silent *omission*, which reflection structurally cannot do.
v2 additionally turns the whole "type not handled" case into a single
actionable static_assert, because the dispatch is local to the serializer
instead of relying on the library's (long) overload-set error paths.

### 2.3 The correction: private members ARE reflectable

The project's earlier M2 conclusion claimed `basic_json::json_value` (private
nested union) is unreachable from outside, forcing a "mirror union". That was
**wrong**, and the evaluation operation that caught it is worth sharing:

```cpp
// consteval context — works
consteval auto json_value_info() {
    constexpr auto m_data = std::meta::nonstatic_data_members_of(
        ^^json, std::meta::access_context::unchecked())[0];      // data
    constexpr auto m_value = std::meta::nonstatic_data_members_of(
        std::meta::type_of(m_data), std::meta::access_context::unchecked())[1]; // m_value
    return std::meta::type_of(m_value);                            // json_value
}
using real_json_value = typename [: json_value_info() :];
```

Re-verified by `probe_real_json_value.cpp` (passes; prints all 8 members):
`object, array, string, binary, boolean, number_integer, number_unsigned,
number_float`.

| operation | result |
|---|---|
| direct `[: ^^ json::json_value :]` | ✗ `is private within this context` |
| `unchecked()` enumerate private data members | ✓ |
| `type_of` → private nested type | ✓ |
| enumerate its members in a **consteval fn** | ✓ (8 members) |
| enumerate same inside **`template for` inline** | ✗ `not a complete class type` (GCC 16) |
| splice construct / read-write / alloc-free | ✓ (ASan clean) |

**This is the single most valuable evaluation finding**: the "can't reflect
private nested types" conclusion was an artifact of the `template for` inline
context, not a reflection limitation. Same code in a consteval function works.
Probe: `tests/static-reflection/probe_real_json_value.cpp`.

Consequence for the implementation: the schema tables (`kStorage`,
`kMemberIds`) in `reflection_json.hpp` are now generated **from the real
`basic_json::json_value`**, with a `static_assert` tying the local storage union
to the real member list — mirror drift is now a compile error instead of a
silent mismatch. The local plain union is kept only as a trivial storage carrier
(no ctor ⇒ no heap-allocation contract), which is a deliberate, documented
choice.

### 2.4 Cost of the "reflect the real private union" switch (measured)

After switching the schema source from the mirror union to the real
`json_value` (consteval indirect route + static_assert), we re-measured
`m2_diff.cpp` compiled against the old header (`git show
7f37cc7f^:include/nlohmann/reflection_json.hpp`) vs the current one, same TU,
`-std=c++26 -freflection`, g++-16, min of 3:

| | old (mirror) | new (real + static_assert) |
|---|---|---|
| compile wall @ -O0 | 1.93 s | 1.93 s (within noise) |
| compile wall @ -O2 | 2.06 s | 2.10 s (within noise) |
| peak RSS @ -O0 | ~354 MB | ~354 MB (+0.1%) |
| executable @ -O0 | 172,640 B | 172,640 B (**identical**) |
| executable @ -O2 | 27,600 B | 27,600 B (**identical**) |
| text @ -O0 / @ -O2 | 64,673 B / 16,175 B | identical |
| behavior | M2 DIFF TEST PASSED | PASSED; ASan clean (exit 0) |

**Takeaway**: reflecting the real private nested union costs ~nothing at
compile time and nothing at runtime — the schema is `constexpr`, so both
sources produce the same tables, and the `static_assert` is evaluated away.
The only real cost of this design is the consteval plumbing (the
consteval-only enumeration trap), not time or size. These numbers reproduce the
previous report **exactly** (byte-identical executables both -O levels). This
strengthens the case for preferring the real type over a mirror whenever the
type is reachable.

---

### 2.5 The ADL-aware recursive codec: probe results

`tests/static-reflection/probe_adl_recursion.cpp` verifies the three paths the
v2 design depends on (18 checks, all PASS, ASan clean at -O1):

- **[A] Nested plain structs** — struct-in-struct, `vector<PlainStruct>` and
  `map<string, PlainStruct>` members serialize and round-trip through the
  reflection/container branches (exact-json and reserialize checks).
- **[B] ADL customization priority** — a reflectable struct (`mine::Date`, 3
  public members) with a user `to_json` serializes as the user's string form,
  not as a reflected object; an explicit `nlohmann::adl_serializer`
  specialization works too; nested custom types (a `Date` member and a
  `vector<Date>` member inside a reflected `Event`) are caught by the adl
  branch — the §2.1 recursion pitfall is fixed — and output is byte-equal to
  native `nlohmann::to_json` for the same values.
- **[C] Private members** — `codec<false>` (unprivileged) serializes only the
  2 public members of a class with a private field, and `from_json` leaves the
  private member untouched; `codec<true>` (unchecked) serializes all 3. The
  classification facts are also static_asserted: a private-only class is
  reflectable only under unchecked(), and member counts are 2 vs 3.
- **[E] v1 parity** — on flat structs, v2 output is identical to the v1 naive
  serializer (no regression on the case v1 could already handle).

Plus the two traps that shaped the design (see §2.1): the nested-json
string_like trap and the from_json container over-acceptance, both
reproduced and worked around by the priority ordering.

Two negative facts worth recording: a `json` VALUE is adl-viable to nlohmann's
own traits (via the string_like trap) but must be copied natively — the
nested-json branch precedes the adl branch; and the v2 dispatch is local, so
an unsupported type yields a single actionable static_assert instead of the
library's 238–376-line error cascade.

## 3. Evaluation methodology takeaways

1. **Before/after must hold the standard AND flags fixed, at the TARGET
   standard** — comparing c++11 vs c++20 misattributes the standard-upgrade
   cost (here +9.6%, not +3%) to the concepts change; pinning everything to
   c++20 hides the C++26 + `-freflection` target (flag cost +3.2%, not +7–8%).
   The headline comparison for a C++26 modernization is before (develop) vs
   after (branch) at `-std=c++26 -freflection`; the full matrix then isolates
   each factor. Real full cost: ≈0–1%.
2. **State the optimization level with every compile-time/binary number AND
   keep the generated code observable** — the reflection-vs-macro gap did not
   collapse at -O2 once the benchmark prevented dead-code elimination: +24–58%
   exe at -O2 (growing with N) vs +23–27% at -O0. A benchmark whose -O2 build
   eliminates the code under test measures nothing.
3. **Compare failure modes, not just success — but verify what "silent" means** —
   for a listed unserializable member both macro and reflection paths fail at
   compile time; the real macro hazard is *omission* (a member not listed in
   `NLOHMANN_DEFINE_TYPE_INTRUSIVE` is silently dropped from serialization with
   zero diagnostics), which reflection structurally cannot do.
4. **Non-default template parameters are mandatory in the probe matrix** — the
   concepts parameter-order bug was invisible with `std::string`.
5. **Consteval vs `template for` context changes what GCC lets you reflect** —
   if a reflection query fails in one context, retry it in a consteval helper
   before concluding it is impossible.
6. **`-freflection` must not go into `CMAKE_CXX_FLAGS`** — it leaks into nested
   subproject `TryCompile` probes that compile with the default standard and
   fail. (Preset fix + AGENTS.md rule.)

## 4. Reproduce

```sh
# concepts vs enable_if (compile time / .o size; min of 3 runs is the number)
FLAGS="-Wno-deprecated -Wno-float-equal -Wno-deprecated-declarations
       -DDOCTEST_CONFIG_SUPER_FAST_ASSERTS -DJSON_TEST_KEEP_MACROS
       -DJSON_TEST_USING_MULTIPLE_HEADERS=1 -Itests/thirdparty/doctest
       -Itests/thirdparty/fifo_map"
git worktree add /tmp/json-baseline develop          # cdf52ae9
/usr/bin/time -f "wall=%e s" g++-16 -O0 -std=c++26 -freflection $FLAGS \
  -I/tmp/json-baseline/include -c tests/src/unit-serialization.cpp -o /tmp/b.o
/usr/bin/time -f "wall=%e s" g++-16 -O0 -std=c++26 -freflection $FLAGS \
  -Iinclude -c tests/src/unit-serialization.cpp -o /tmp/a.o

# reflection vs macro — three modes (the checked-in benchmark TU, §2.2)
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
  -o /tmp/bm tests/static-reflection/bench_macro_vs_reflection.cpp      # macro
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_REFLECTION -Iinclude \
  -o /tmp/br tests/static-reflection/bench_macro_vs_reflection.cpp      # refl v1
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_ADL_REFLECTION -Iinclude \
  -o /tmp/br2 tests/static-reflection/bench_macro_vs_reflection.cpp     # refl2 v2
size /tmp/bm /tmp/br /tmp/br2 && nm /tmp/bm /tmp/br /tmp/br2 | wc -l

# the ADL-aware recursive codec probe (§2.5): nested / ADL / private / parity
g++-16 -std=c++26 -freflection -O0 -Iinclude \
  -o /tmp/par tests/static-reflection/probe_adl_recursion.cpp && /tmp/par

# real private json_value probe + differential (behavior parity + ASan)
g++-16 -std=c++26 -freflection -O0 -Iinclude \
  -o /tmp/prjv tests/static-reflection/probe_real_json_value.cpp && /tmp/prjv
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
```

All probes live in `tests/static-reflection/`; each file's header documents its
build line. Numbers above were captured on the branch's machine (GCC 16.1.0,
x86-64, no ccache); absolute values vary by hardware, **ratios and
methodology are the point**.

## 5. Measurement log & corrections vs the previous version

Re-measured 2026-08-15 on this machine (12th Gen i5-12600KF, 10 cores,
g++-16 16.1.0-2ubuntu1, no ccache; baseline develop `cdf52ae9`, branch
`cca6c3c5`). Raw runs (3× per config, min reported) are in the session's
`/tmp/eval-20260815/results/{matrix1,matrix2,matrix3,runtime,probes}.txt`
(uncommitted working artifacts).

Three-mode re-measurement (this session, same machine, same toolchain; the
sweep re-ran macro/v1 together with the new v2 mode, min of 3; raw runs in
`build/scratch/measure_results.txt`, uncommitted): the macro and v1
executable sizes reproduce the previous report exactly at every N (e.g. N=50
-O0: 530,088 / 669,208; N=100 -O2: 266,640 / 421,720); the new v2 mode is
**byte-identical to macro at -O2** at every N (113,384 / 131,088 / 145,120 /
182,608 / 266,640) and cheaper to compile than v1 (−6…−24% at -O2). §2.2's
tables now carry the three-mode numbers; the previous two-mode table rows for
macro/v1 are superseded by the same-session values.

Claims that **reproduce exactly** (stable facts):
- Diagnostics counts: 238 vs 376 lines, 27,987 vs 42,478 bytes, identical first
  error; the "why" lines in the concepts output.
- `.o` sizes for unit-serialization.cpp at all standards (2,555 / 2,580 / 2,595 /
  2,612 / 2,628 KB) — match the previous report's decimal-KB values.
- §2.4 old-vs-new m2_diff: byte-identical executables (172,640 B / 27,600 B),
  RSS ~354 MB, compile within noise, ASan clean.
- §2.3: 8 members of the real `json_value`, consteval-only enumeration trap,
  splice usage — probe passes.
- §1.4: alt-string parameter-order trap and the `<T, B>` fix (minimal repro).
- All 16 probes build and pass per their documented build lines;
  `concepts_smoke` output identical under c++11/20/26.

Claims that **did not reproduce** and were corrected:
- §1.2 decomposition: concepts slice was −5% → now ≈0%; `-freflection` flag
  +7–8% → +3.2%; standard upgrade +3% → +9.6%. Wall absolutes also shifted
  (3.77/3.74 s → 3.55/3.59 s) — machine conditions; ratios within the same
  "≈0 full cost" conclusion.
- §2.2 reflection-vs-macro numbers (compile, binary, break-even): the old TU
  was never checked in, so nothing there was reproducible; replaced by the
  checked-in `bench_macro_vs_reflection.cpp` with a full N × O-level sweep.
  Break-even moved from "28 lines / 28 types" to "25 lines / 25 types" (the
  actual serializer line count).
- §2.2 "binary gap collapses at -O2 (+90% → +12%)" and "same symbol count (8)":
  not reproduced — with observable codegen the -O2 gap is *larger* at scale
  (+58% exe at N=100); `nm` shows more symbols for reflection (+10–26%).
- §2.2 Q4 "macros fail silently at runtime": corrected — both paths compile-error
  for a listed unserializable member; the genuine silent failure is macro
  *omission*, which reflection cannot do.

The lesson is the point of this document: with a long-running evaluation, keep
the raw runs and the exact TUs — conclusions in prose survive, numbers do not.
