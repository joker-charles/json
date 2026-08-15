# Reflection vs Macros vs Concepts: Measured Evaluation Notes

> **Branch**: `feature/static-reflection` (nlohmann/json 3.12.0 + experimental work)
> **Toolchain**: GCC 16.1.0 (`g++-16`), flags `-std=c++26 -freflection` (reflection),
> `-std=c++20` (concepts), `-std=c++11` (baseline enable_if)
> **Status**: re-measured from scratch on 2026-08-15 (this machine, 12th Gen
> i5-12600KF, no ccache, g++-16 16.1.0-2ubuntu1); raw runs in the measurement
> log (§5). Every number below was re-derived in that session — the previous
> version of this document carried numbers that did **not** survive re-measurement;
> §5 lists exactly which claims were corrected.

> **Applicability.** Every number below is single-sample evidence: one
> compiler (GCC 16.1.0, `g++-16`), one library (nlohmann/json 3.12.0), one
> machine (12th-gen i5-12600KF, x86-64), one data shape (person-like flat
> and shallow-nested structs with string/numeric/container members), and
> the flags listed per section (`-std=c++26 -freflection`; `-O0`/`-O2`).
> Absolute values — and some ratios — may differ substantially on other
> compilers (Clang/MSVC), other library versions, or other type shapes;
> the durable findings are the **methodology** and the **same-toolchain
> relative relationships** (e.g. on flat/shallow-nested types refl2 is
> never slower than the macro baseline at runtime and size-identical at
> -O2; the extended inheritance/optional/bit-field paths are measured in
> §2.2 and do not collapse quite as far).

**Key findings** (quick reference — measurements and caveats in §1/§2):
- **Concepts are free.** The full modernization (concepts + reflection-
  ready) costs ≈0–1% wall / ≤0.6% `.o` at the target standard; the
  concepts slice alone ≈0%.
- **Reflection saves 1 line per type** and cannot silently omit members
  (the macro's omission hazard is structurally impossible). If the library
  ships the codec (scenario B), the user's one-time cost is 0 — every type
  breaks even from the first one; writing it yourself (scenario A) costs
  v1 = the naive reflection serializer (25 lines, flat structs only) /
  v2 = the complete ADL-aware recursive codec "refl2" (667 lines whole
  file, 463 code-only), break-even ≈ 25 / ≈ 667 (≈ 463) types. (Definitions:
  §2.1.)
- **At -O2 the flat path is size-identical to the macro version** (exe
  equal at every N, text within 32 B). The extended
  inheritance/optional/bit-field paths do NOT collapse that far: text
  +1.9% / exe +5.5% for a three-type TU (+11.6% per inherited/optional
  single-type TU; bit-fields −2.2%).
- **Runtime: never slower than macro on flat/shallow-nested types**
  (nested ~13–19% faster). Extended paths: deserialize 20–37% faster,
  but serialize 5–6% slower for inherited/optional types.
- **Unsupported types get one actionable static_assert** (11 error lines)
  instead of the library's 238 (enable_if) / 376 (concepts) error
  cascades; the v2 dispatch also collapses at -O2 to ~macro code size on
  flat types.
- **v2 is cheaper to compile than v1** (−1.7…−3.9% at -O0; −7.7…−21.2%
  at -O2); -O2 wall times vs macro are load-noise on this machine and are
  not used for conclusions.
- **Single-toolchain evidence** — g++-16 / nlohmann-json 3.12.0 / one
  machine / person-like shapes: ratios and methodology are the durable
  findings (see Applicability), not the absolutes.

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
in an earlier revision and optimized in this one (mode
`-DBENCH_ADL_REFLECTION`). The codec is a standalone
header, `tests/static-reflection/refl2_codec.hpp` (the single source of truth
shared by the probe `probe_adl_recursion.cpp`, the compile bench
`bench_macro_vs_reflection.cpp` and the runtime bench `bench_runtime.cpp`).
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
- One-time cost: the header is **667 lines** total — 463 lines of code
  (including the 10 `#include`/`#pragma once` lines), 148 comment lines (89
  of them the design/pitfall/coverage documentation block), 56 blank lines.
  This replaces the earlier inline `namespace refl2` block (311 lines + 1
  `#include <meta>` = 312); the increase buys the optimization machinery
  below, the `std::optional` branch, the inheritance (subobjects_of)
  recursion with its guards, bit-field from_json support and the documented
  coverage boundary.

**Optimizations in this revision** (measured, see §2.2 runtime):
- **Member keys are one-time-initialized `static const std::string`**
  (function-local magic statics, thread-safe init, arbitrary length) instead
  of a per-call `std::string(identifier_of(m))` — removes the per-member
  runtime key construction and its call-site codegen. A constexpr
  `std::array<std::string, N>` key table was tried first and **rejected**: on
  this toolchain (g++-16, libstdc++ 16) it constant-initializes only for SSO
  keys (≤15 chars); longer keys fail with `refers to a result of 'operator new'`.
- The member loop switched from `template for` to an `std::index_sequence`
  pack expansion over consteval variable templates (`member_v<U,T,I>`,
  `member_key_v<U,T,I>`, `member_count<U,T>`) — the pattern this branch
  verified on GCC 16; still no `if constexpr`-with-splices anywhere.
- The array branch serializes directly into `j.emplace_back()` (which returns
  a reference to the new element) instead of a temp `json` + `push_back`.
- **Diagnostic hardening**: `adl_branch_eligible` now excludes C arrays unless
  string-like (`!(std::is_array<T>::value && !is_string_like<B,T>)`) — a plain
  C array no longer falls into nlohmann's C-array `to_json` path and dies with
  a deep library error; it hits the clean priority-0 `static_assert` instead
  (`char[N]` still serializes as a string).

**Inheritance is supported**: base-class members are serialized.
`nonstatic_data_members_of` sees only direct members, so the member facts
are computed from `subobjects_of` (direct base subobjects + direct data
members, access-filtered, bases first) with a consteval recursion into each
base via `type_of(base_info)` — multi-level, depth-first, in layout order;
the member-access splice `v.[:m:]` works for members of base classes too
(verified). Three shapes are compile errors instead of silent loss:
**private bases** and **protected bases** under `codec<false>` — distinct
messages via `is_private` / `is_protected` on the unchecked base list (use
`codec<true>` or add a `to_json`) — and **virtual bases** (`is_virtual`): a
shared virtual subobject would flatten its members multiple times while C++
has one such subobject, so the members would wrongly duplicate
(deduplication not implemented). **Duplicate member names across the
hierarchy** (same name in a base and a derived class, or diamond
inheritance) are also compile errors — they would silently overwrite JSON
keys. **Bit-fields**: named bit-fields serialize normally (verified) and
`from_json` assigns them via `get<M>()` (they cannot bind to a `T&` — no
address, same limitation as the macro path); unnamed bit-fields are not
subobjects and are skipped.

**`std::optional<T>` is fully supported** via its own dispatch branch
(priority 5): it serializes as `T` / `null` for any refl2-serializable `T`
(reflectable structs, containers, nested json, ...) — `optional<PlainStruct>`
now round-trips. The `is_optional` exclusion from `is_array_like` is still
required: C++23 added `begin()`/`end()` + `value_type` to `std::optional`,
which would otherwise misclassify it as array-like and silently serialize
`optional<int>{5}` as `"[5]"` (verified before the fix).

**Coverage boundary** (what v2 deliberately does NOT handle — such types fall
to the priority-0 `static_assert` with an actionable message; each extension
below raises the one-time cost beyond the current 667 lines):
- **unions; `std::variant`** (would need `variant_size`/`variant_alternative`
  integration, ~20 lines, plus a discriminator format design).
- **pointers / self-referential types**.
- **Ranges with `begin`/`end` but no `value_type` member**: NOT excluded —
  they fall through to the adl branch (nlohmann's range-array path accepts
  any begin/end type at detection level), so a const-iterable compatible
  range serializes as an array, while an incompatible one (e.g. non-const-
  iterable) dies with a deep library error rather than the clean
  static_assert.
- **non-default-constructible container elements** (`from_json` needs
  `value_type{}`); **map keys other than string-like / arithmetic**.
- C arrays: not supported, but now a clean compile error (string-like
  `char[N]` still works).

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
-freflection`; compile times: -O0 min of 3, -O2 median of 7; raw runs
committed in §5). All three modes print the same
counter — the JSON output is identical — so the numbers are pure serializer
cost, not behavior drift.

**Q1 code saved** — reported in **two scenarios**, because the break-even
point answers different questions depending on who pays the one-time cost:

- **Scenario A — the user writes the serializer themselves** (the status quo
  of this evaluation: the codec is a header the user copies into their
  project). Per-type user lines: macro = struct + macro call = 2/type;
  reflection = struct only = 1/type; one-time cost: v1 = 25 lines
  (flat structs only), v2 = 667 lines (refl2_codec.hpp, whole file; the
  code body alone is 463 lines — 148 comment / 56 blank lines are the
  design/pitfall/coverage documentation, see §2.1; counting only code moves
  the v2 break-even to ≈ 463 types).

  | types | macro | refl v1 | refl2 v2 |
  |---|---|---|---|
  | 1 | 2 | 26 | 668 |
  | 20 | 40 | 45 | 687 |
  | **25** | 50 | 50 | 692 |
  | 50 | 100 | 75 | 717 |
  | 100 | 200 | 125 | 767 |
  | **667** | 1334 | 692 | 1334 |

  Both reflection versions save **1 line per type**; the naive v1 needs a
  **25-line** one-time serializer (break-even ≈ 25 types) but only works for
  flat structs, while the complete v2 (recursion + ADL + private-member
  policy + inheritance + `std::optional` + bit-fields + the documented
  coverage boundary) needs a **667-line** one-time serializer (break-even
  ≈ 667 types) and handles nested structs, containers of plain structs,
  user customization, base-class members and `unprivileged()` access
  control — which v1 cannot do at all.

- **Scenario B — the library ships v2 as a default facility** (like the
  macros today): the user's one-time cost is **0**, so every type saves 1
  line **from the very first type** (break-even = 1 type):

  | types | macro | refl2 v2 | saved |
  |---|---|---|---|
  | 1 | 2 | 1 | 1 |
  | 20 | 40 | 20 | 20 |
  | 100 | 200 | 100 | 100 |

  The 667-line codec then becomes a **maintainer cost** — paid once by the
  library, amortized over all users — not a per-user cost. Scenario A's
  "312 types to break even" framing was misleading: it only describes users
  who re-implement the serializer by hand; for the library as a provider the
  user-side cost is zero from the first type, and what the library actually
  ships is the 667-line implementation (plus the extension burden documented
  in §2.1's coverage boundary).

**Q2 compile time** (same TU, same flags; -O0: min of 3 — stable; -O2:
median of 7 — high-noise, see the note below):

| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 2.21 s | 2.35 s | 2.29 s | 3.17 s | 3.23 s | 2.98 s |
| 20 | 2.41 s | 2.59 s | 2.49 s | 3.18 s | 3.52 s | 2.99 s |
| 28 | 2.50 s | 2.66 s | 2.57 s | 3.26 s | 4.17 s | 3.53 s |
| 50 | 2.69 s | 2.98 s | 2.93 s | 3.79 s | 4.87 s | 3.89 s |
| 100 | 2.77 s | 3.19 s | 3.13 s | 5.01 s | 7.13 s | 5.62 s |

v2 is **cheaper to compile than v1** at every measured point (−1.7…−3.9% at
-O0; −7.7…−21.2% at -O2, medians of 7) and close to the macro baseline at
-O0 (+2.8…+13.0%, min of 3). The dispatch machinery is resolved at compile
time and adds less per-type instantiation work than v1's per-member
`j[...] = v.[:m:]` + `std::string(identifier_of(m))` codegen.

**-O2 compile time is not reliable on this machine.** The -O2 wall times
were re-measured with 7 compiles per config (medians above; raw values in
§5's snapshot) and still swing −6.0…+12.2% vs macro across N (v2 faster at
N=1/20, slower at N=28/100) under machine load. The earlier min-of-3 run
swung −10.6…+15.0% — same picture, different bounds. The robust evidence in
this evaluation is the **-O0 compile time** (stable), the **-O2 binary size
identity** (Q3) and the **runtime throughput** below; the -O2 compile-time
column should be read as indicative only.

**Q3 binary** (executable bytes / `size` text, per N):

| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |
|---|---|---|---|---|---|---|
| 1 | 393,656 / 169,213 | 484,512 / 199,086 | 395,632 / 169,997 | 113,384 / 80,111 | 119,888 / 85,244 | 113,384 / 80,143 |
| 20 | 445,936 / 202,548 | 555,608 / 260,100 | 501,944 / 217,432 | 131,088 / 94,192 | 174,456 / 129,294 | 131,088 / 94,224 |
| 28 | 470,560 / 216,580 | 584,264 / 285,788 | 546,544 / 237,400 | 145,120 / 106,241 | 194,248 / 149,994 | 145,120 / 106,273 |
| 50 | 530,088 / 255,186 | 669,208 / 356,438 | 666,120 / 292,319 | 182,608 / 138,326 | 265,248 / 207,914 | 182,608 / 138,358 |
| 100 | 667,600 / 343,012 | 850,360 / 517,124 | 939,752 / 417,253 | 266,640 / 208,925 | 421,720 / 342,974 | 266,640 / 208,957 |

**The headline**: at **-O2, v2 is size-identical to the macro version** — the
executable size is exactly equal at every N (113,384 / 131,088 / 145,120 /
182,608 / 266,640 B), `size text` is within 32 B (a read-only alignment gap;
macro 94,192 vs v2 94,224 at N=20), and `nm` is identical to macro at every
N (179 / 206 / 215 / 238 / 288 symbols; v1: 191 / 239 / 255 / 300 / 400).
True byte-identity is not expected: the linker build-id note hashes the TU
content, and the two sources necessarily differ. The v2 dispatch
(priority_tag ranking, adl_serializer indirection, reflection recursion)
fully collapses under optimization: both paths converge on the same
`adl_serializer`/`external_constructor` code the macro generates directly.
v1 does NOT converge (its per-member string-key conversion + assignment
survives optimization: +6…+58% exe at -O2) — so v2 is **smaller than v1 at
-O2 by −5…−37%**.

The static-key optimization (§2.1) does **not** break size identity: the
`static const std::string` keys are constant-folded into `.rodata` and
identical keys merge (the binary holds exactly one copy of each member name,
same as the macro's literals), so `.rodata` grows by only the same 32-B
alignment gap and the executable size is unchanged at every N.

At **-O0**, v2 sits between macro and v1: near-macro at N=1 (+0.5%), growing
to +40.8% exe at N=100 (v1: +27.4%). The -O0 overhead is the un-collapsed
dispatch: `nm` at N=50 counts 1,871 symbols for v2 vs 1,621 (v1) / 1,471
(macro). The old §2.2 lesson still stands — always state the optimization
level and keep the generated code observable — but the new finding is that
the -O0 bloat of v2 is *mostly optimization artifacts*: it evaporates at -O2
to exactly zero.

**Runtime (conversion layer)** — new in this revision:
`tests/static-reflection/bench_runtime.cpp`, three modes selected by the same
`-DBENCH_*` flags. Flat person (5 members) in all three modes; nested person
(struct-in-struct with `vector<Address>`) in macro vs v2 (v1 cannot serialize
nested types — skipped). Directions: `to_json` / `from_json` / round-trip.
Method: warmup + 7 timed runs × 200k iterations (`BENCH_RUNS`/`BENCH_ITER`
overridable), median us/op reported; the input varies with the loop index
(`age += i%7`, `name += char`) and deserialize polls 8 pre-built json sources
(`i%8`), so no work can be hoisted or eliminated at -O2; every iteration
feeds a printed sink counter. `-O2`, seed 12345, min/median of 7 binary runs:

| direction | macro | refl v1 | refl2 v2 (old codec) | refl2 v2 (optimized) |
|---|---|---|---|---|
| flat serialize | 0.688 / 0.693 | 0.588 / 0.590 | 0.659 / 0.696 | 0.658 / 0.665 |
| flat deserialize | 0.110 / 0.112 | 0.101 / 0.103 | 0.111 / 0.115 | 0.103 / 0.105 |
| nested serialize | 1.396 / 1.398 | — (v1 n/a) | 1.199 / 1.258 | 1.206 / 1.215 |
| nested deserialize | 0.243 / 0.248 | — (v1 n/a) | 0.212 / 0.225 | 0.197 / 0.201 |

Scope: `dump`/`parse` are library code, byte-identical across modes — the
table measures only the **conversion layer** (DOM build / DOM read).
Round-trip (`to_json` + `dump` + `parse` + `from_json`) is an end-to-end
check; the full numbers (min/median of 3 runs each, raw samples in §5's
`runtime_measure_20260815.txt`):

| mode | flat round-trip (min / median) | nested round-trip (min / median) |
|---|---|---|
| macro | 2.339 / 2.352 us/op | 4.964 / 5.040 us/op |
| refl v1 | 2.201 / 2.221 us/op | — (v1 n/a) |
| refl2 v2 (old codec) | 2.430 / 2.438 us/op | 5.040 / 5.071 us/op |
| refl2 v2 (optimized) | 2.357 / 2.358 us/op | 4.799 / 5.032 us/op |

All modes land in the same band (flat ≈ 2.2–2.5 us/op, nested ≈ 4.8–5.15
us/op): the end-to-end time is dominated by `dump`/`parse` (identical
library code), and the mode-to-mode differences (≤ ~9% on flat, <2% on
nested — the old-codec flat median 2.438 vs v1 2.221 is the largest) are a
small fraction of the conversion-layer component — no end-to-end regression.

Reading the numbers:
- refl2 v2 is **never slower than the macro baseline on the flat and
  shallow-nested bench types**, and on the nested
  directions it is **~13–19% faster** (medians: nested serialize 1.215 vs
  1.398; nested deserialize 0.201 vs 0.248). Plausible cause: v2's pre-built
  static keys vs the macro's per-call literal→`std::string` construction —
  each nested op touches 13 member keys (4 + 3×3), so the saving compounds.
- v1's flat serialize is **~15% faster than macro** (0.590 vs 0.693): the
  naive direct-assignment path (`j[...] = v.[:m:]`) is cheaper than the
  library's object-construction path for the same output. Recorded as an
  observation, not forced into an explanation.
- Optimized vs old codec (the static-key / index_sequence rework): the
  medians improve by ~4–10% (flat serialize 0.665 vs 0.696, flat deserialize
  0.105 vs 0.115, nested serialize 1.215 vs 1.258, nested deserialize 0.201
  vs 0.225) — the biggest relative gain is on deserialize, where `at(key)` is
  called per member.
- The earlier "exact-equal minima" across v1/old/new (0.553/0.095) were an
  artifact of a measurement-script bug (an associative array not reset
  between binaries — minima accumulated across modes); with the fixed script
  each mode reports its own min/median (see §5).

Absolute values are machine-specific; ratios within the same binary run are
the point.

**Extended paths (inheritance / std::optional / bit-fields)** — the new
dispatch branches (§2.1) measured with
`tests/static-reflection/bench_extended.cpp`: a multi-level `DerivedPerson`
(4 members across 3 levels), an `OptionalPerson` (`std::optional<Address>`
member, toggled empty/has-value by the loop index), and a `BitFieldStruct`
(two named bit-fields + one int), each in macro vs refl2 mode with identical
flags (-O2). Macro baselines: inheritance lists the base-class public members
in the macro (legal — the expansion is `j["base_name"] = v.base_name`);
optional uses nlohmann's native optional support; bit-fields have NO macro
`from_json` (`get_to` needs a `T&`, a bit-field has no address) so the
baseline is a hand-written `get<M>()`-assignment serializer — the "ideal
macro" shape. Both modes print identical totals (parity).

**Binary (-O2)**:

| TU | macro exe / text | refl2 exe / text | delta |
|---|---|---|---|
| all three types | 230,728 / 175,900 | 243,432 / 178,260 | exe +12,704 (+5.5%) / text +2,360 (+1.3%) |
| inherited only | — / 65,834 | — / 73,468 | text +7,634 (+11.6%) |
| optional only | — / 66,336 | — / 74,035 | text +7,699 (+11.6%) |
| bitfield only | — / 71,232 | — / 69,687 | text −1,545 (−2.2%) |

Unlike the flat path (exe exactly equal, text within 32 B), the extended
paths do NOT collapse to byte-identity with their macro baselines: the
inheritance and optional dispatch adds ~7.6 KB text each in a single-type TU
(+11.6%), and ~2.4 KB text / ~12.7 KB exe for the full three-type TU
(+1.9% / +5.5% — the shared framework amortizes the per-type overhead). The
bit-field path is *smaller* than the hand-written baseline (−1.5 KB, −2.2%):
refl2's `get<M>()` assignment compiles tighter than the initializer-list
baseline. Section attribution: `.text` +2.4 KB (the dispatch framework),
`.data` +0.8 KB (12 static key objects vs the macro's string literals).

**Runtime** (min/median of 5 binary runs, -O2, seed 12345, us/op):

| direction | macro | refl2 |
|---|---|---|
| inherited serialize | 0.276 / 0.279 | 0.295 / 0.297 |
| inherited deserialize | 0.076 / 0.078 | 0.060 / 0.061 |
| inherited round-trip | 1.473 / 1.473 | 1.391 / 1.394 |
| optional serialize | 0.276 / 0.278 | 0.289 / 0.291 |
| optional deserialize | 0.066 / 0.066 | 0.051 / 0.053 |
| optional round-trip | 1.296 / 1.307 | 1.249 / 1.251 |
| bitfield serialize | 0.540 / 0.548 | 0.212 / 0.213 |
| bitfield deserialize | 0.054 / 0.054 | 0.034 / 0.034 |
| bitfield round-trip | 1.242 / 1.249 | 0.886 / 0.890 |

Reading: deserialize is consistently faster (inherited −22%, optional −20%,
bit-field −37% on the medians) and round-trip is faster or equal — but
**serialize is 5–6% slower for inherited/optional** (0.297 vs 0.279; 0.291
vs 0.278): the extended dispatch does not collapse quite as well as the flat
path on the to_json side, while from_json's member loop wins. Bit-fields
serialize 61% faster — but the hand-written initializer-list baseline is the
slow shape, not the typical macro one. **The flat-path headline "never
slower than macro" does NOT transfer unchanged**: the extended paths are
faster on deserialize/round-trip and ~5–6% slower on serialize for
inherited/optional types.

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

`tests/static-reflection/probe_adl_recursion.cpp` verifies the paths the
v2 design depends on — it includes the shared `refl2_codec.hpp` (no inline
copy) and runs **38 checks, all PASS, ASan clean at -O0 and -O1**:

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
- **[D] `std::optional`** — dedicated dispatch branch, never array-like:
  `optional<int>` serializes byte-equal to native nlohmann (`5` / `null`,
  round-trip verified), and `optional<Address>` (a plain struct) now
  round-trips through the reflection recursion (object / `null` /
  `nullopt`).
- **[F] inheritance** — base-class members are serialized via the
  `subobjects_of` recursion: a multi-level `Derived : Mid : Base` round-trips
  with all four members (verified against the exact JSON); the compile-time
  guards are trait-checked: private vs protected bases are classified with
  `is_private` / `is_protected`, virtual bases are detected (`is_virtual`),
  duplicate member names across a diamond hierarchy are detected, and
  `codec<true>` serializes private-base members too.
- **[G] toolchain facts** — `is_enumerable_type` pre-checks the `bases_of`
  enumeration trap (true for a complete class, false for an incomplete
  type); bit-fields are reflectable and not array-like, named bit-fields
  serialize and `from_json` round-trips them (`get<M>()` assignment — they
  cannot bind to a `T&`), and the unnamed bit-field is skipped.
- **[E] v1 parity** — on flat structs, v2 output is identical to the v1 naive
  serializer (no regression on the case v1 could already handle).

Plus the two traps that shaped the design (see §2.1): the nested-json
string_like trap and the from_json container over-acceptance, both
reproduced and worked around by the priority ordering. New in this revision,
the dispatch-classification checks also cover **C arrays**: `char[5]` stays
on the string path while a plain C array (`Address[2]`) is classified as
neither adl-eligible nor reflectable — it hits the clean priority-0
`static_assert` instead of nlohmann's C-array `to_json` path (which breaks on
non-constructible elements), plus the scalar/string/container trait facts
and the private-only/unchecked() classification from §2.3.

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
7. **Single-toolchain evidence — state the scope with the numbers.** Every
   number here is g++-16 / nlohmann-json / one machine / one type shape;
   the durable claims are the methodology and the same-toolchain
   relationships, not the absolutes (see "Applicability" in the header).
   And: wall-clock compile times at -O2 are load-sensitive and should be
   reported with their noise or dropped (see §2.2 Q2) — binary size and
   runtime measurements are the stable evidence.

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
# full §2.2 sweep (N x O-level x mode, min of 3, ~10 min):
bash tests/static-reflection/bench_macro_vs_reflection.sh
# quick smoke (single N, single run) + paste-ready §2.2 markdown tables:
BENCH_N_SET="50" BENCH_RUNS=1 BENCH_MARKDOWN=1 \
  bash tests/static-reflection/bench_macro_vs_reflection.sh
# or the minimal per-mode builds:
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
  -o /tmp/bm tests/static-reflection/bench_macro_vs_reflection.cpp      # macro
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_REFLECTION -Iinclude \
  -o /tmp/br tests/static-reflection/bench_macro_vs_reflection.cpp      # refl v1
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_ADL_REFLECTION -Iinclude \
  -o /tmp/br2 tests/static-reflection/bench_macro_vs_reflection.cpp     # refl2 v2
size /tmp/bm /tmp/br /tmp/br2 && nm /tmp/bm /tmp/br /tmp/br2 | wc -l

# runtime throughput — conversion layer (§2.2); three modes, -O2
g++-16 -std=c++26 -freflection -O2 -Iinclude \
  -o /tmp/brt tests/static-reflection/bench_runtime.cpp                    # macro
g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_REFLECTION \
  -o /tmp/brt1 tests/static-reflection/bench_runtime.cpp                   # refl v1
g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
  -o /tmp/brt2 tests/static-reflection/bench_runtime.cpp                   # refl2 v2
# "old codec" baseline for the old-vs-optimized column: the pre-optimization
# codec (extracted from commit 62290f3b, kept as refl2_codec_old.hpp +
# bench_runtime_old.cpp so the "before" side is reproducible):
g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
  -o /tmp/brt_old tests/static-reflection/bench_runtime_old.cpp
# driver (min/median of RUNS=7 binary runs per mode):
bash tests/static-reflection/runtime_measure.sh

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
`cca6c3c5`). The earlier concepts-evaluation raw runs (the §1 matrix/runtime
logs) lived in `/tmp/eval-20260815/results/` and **were not preserved** —
they vanished with the session, which is exactly the failure mode this
section's closing lesson warns about; the §1 numbers survive only in the
tables above and are not re-derivable from repo artifacts. Everything in the
current revision, by contrast, has repo-side drivers and committed raw
snapshots (`docs/static-reflection/data/`, see below).

Three-mode re-measurement (this session, same machine, same toolchain; the
sweep re-ran macro/v1 together with the new v2 mode; raw runs
committed as `docs/static-reflection/data/measure_results_20260815.txt`,
generated by `tests/static-reflection/bench_macro_vs_reflection.sh` — the
`build/scratch/` copy is the regenerable working artifact): the
macro and v1 executable sizes reproduce the previous report exactly at every
N (e.g. N=50 -O0: 530,088 / 669,208; N=100 -O2: 266,640 / 421,720); the new
v2 mode is **size-identical to macro at -O2** at every N (113,384 / 131,088 /
145,120 / 182,608 / 266,640; text within 32 B) and cheaper to compile than
v1 (−7.7…−21.2% at -O2, medians of 7). §2.2's tables now carry the
three-mode numbers; the previous two-mode table rows for macro/v1 are
superseded by the same-session values. The `-O2` wall times are
**load-sensitive and not reliable vs macro** — an extra 7-compile rerun
(medians: v2 −6.0…+12.2% vs macro across N) landed in a different range than
the earlier min-of-3 run (−10.6…+15.0%); the stable facts are the -O0
compile times, the exe/text/nm identity at -O2, and the runtime throughput.

Runtime measurements (this revision, §2.2): `bench_runtime.cpp` in the three
modes, flat + nested, `to_json`/`from_json`/round-trip; warmup + 7 runs ×
200k iterations, median reported; driver `tests/static-reflection/runtime_measure.sh`;
the raw driver output and the round-trip samples are committed as
`docs/static-reflection/data/runtime_measure_20260815.txt`.
The "old codec" baseline is the pre-optimization codec (extracted from
commit 62290f3b) kept in the repo as `tests/static-reflection/refl2_codec_old.hpp` +
`bench_runtime_old.cpp` so the old-vs-optimized column is reproducible.
The driver's first run was invalidated by a script bug: `declare -A samples`
does **not** reset an existing global associative array in bash, so samples
accumulated across the four binaries
and every later mode's min/median was cross-contaminated (v1's "nested" rows
showed macro's exact values, and all modes shared one flat-serialize minimum
0.553/0.582). Fixed with an explicit `samples=()` reset and re-run; the
earlier "exact-equal minima" seen in the draft numbers were this artifact.

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
- §2.5 probe: **38 checks, all PASS, ASan clean at -O0 and -O1** — including
  the C-array classification check and the [D] `std::optional` / [F]
  inheritance / [G] toolchain-fact checks.
- §2.2 runtime: refl2-vs-macro ordering and the old-vs-optimized deltas
  reproduced across the driver runs (nested faster than macro; optimized
  faster than old codec on every direction).

Claims that **did not reproduce** and were corrected:
- §2.1 coverage boundary, `std::optional`: the original entry ("optional<T>
  only when T is adl-able; optional<PlainStruct> falls through") was wrong in
  mechanism and hid a silent correctness bug — C++23 `std::optional` has
  `begin`/`end` + `value_type`, so refl2 classified it as **array-like** and
  silently serialized `optional<int>{5}` as `"[5]"` (native: `"5"`), with
  `optional<PlainStruct>` also array-mangled instead of erroring. Fixed with
  an `is_optional` exclusion from `is_array_like`; probe [D] added (24
  checks); §2.1 reworded. The value_type-less-range entry was also inaccurate
  (such ranges go to the adl branch's range path — compatible ones serialize
  as arrays, incompatible ones die deep in the library, not at the
  static_assert) — §2.1 now states the verified behavior. The bench sweep
  numbers (§2.2 Q2/Q3) were measured with the pre-fix codec; the fix is
  compile-time-only for non-optional types — a N=50 -O2 spot-check rebuilt
  macro and refl2 size-identical (182,608 B each, text within 32 B) with the
  fixed codec.
- §2.1 coverage boundary, **inheritance** (this revision): the boundary entry
  ("base members silently ignored, would need bases_of recursion") is
  obsolete — inheritance is now implemented. Member facts come from
  `std::meta::subobjects_of` (verified: direct bases + direct members,
  access-filtered, bases first) with a consteval recursion into each base
  via `type_of(base_info)` (the direct enumeration of `bases_of` entries
  hits the known "not a complete class type" trap — [LLVM
  #172136](https://github.com/llvm/llvm-project/issues/172136) — the
  `type_of` route works on this toolchain). Private/protected bases and
  duplicate member names across the hierarchy are clean compile errors
  (probe [F]). `std::optional` is now fully supported via its own dispatch
  branch (`optional<PlainStruct>` round-trips; the earlier "hits the
  priority-0 static_assert" note in §2.1 was superseded). The one-time cost
  moved 423 → 565 → 667 lines and Q1 was re-derived each time (break-even
  ≈ 667 types); probe counts 38 checks. Later hardening (same session): the
  private/protected base guard was split into distinct `is_private` /
  `is_protected` diagnostics, virtual bases became a dedicated compile error
  (`is_virtual` — a shared virtual subobject would duplicate its members;
  previously they surfaced as a misleading duplicate-key error), and
  bit-field `from_json` got a `get<M>()`-assignment path (bit-fields cannot
  bind to a `T&`, so the ordinary member path failed to compile; the macro
  path has the same limitation). `is_enumerable_type` was verified as a
  pre-check for the `bases_of` enumeration trap. The bench sweep numbers are
  unaffected (bench types are flat, no optional/inheritance/bit-fields) —
  N=50 -O2 macro vs refl2 re-verified size-identical (182,608 B each) with
  the new codec.
- **Extended-path measurements (inheritance / optional / bit-fields)** (this
  revision): `bench_extended.cpp` closes the coverage gap — the Q3 table only
  covered flat persons and the runtime bench only flat/nested. Findings that
  bound the flat-path headlines:
  - Binary at -O2: the extended paths do NOT collapse to byte-identity with
    their macro baselines — text +1.9% / exe +5.5% for the full three-type
    TU, +11.6% per inherited/optional single-type TU (dispatch framework),
    while bit-fields are −2.2% smaller than the hand-written baseline.
    The flat-path "size-identical at -O2" claim is now scoped to flat types.
  - Runtime: deserialize consistently faster (inherited −22%, optional −20%,
    bit-field −37%), round-trip faster or equal, but **serialize is 5–6%
    slower for inherited/optional** — the flat-path "never slower than the
    macro baseline" claim is scoped to flat/shallow-nested types (header
    note + §2.2 updated). Bit-field serialize is 61% faster than the
    hand-written initializer-list baseline (the baseline shape, not the
    typical macro one).
  - The Q1 Scenario-A break-even is reported with both figures: 667 lines
    (whole file, conservative) and 463 lines (code body only; ≈ 463 types).
- §2.2 "nm counts 288 symbols at N=50": misattributed — the new sweep shows
  238 at N=50 (288 is the N=100 value; macro/v2 identical at every N). The
  claim itself (v2 `nm` == macro at -O2) holds and is now stated for all N.
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
