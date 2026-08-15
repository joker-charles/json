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
The same TU compiles in two modes selected by `-DBENCH_REFLECTION`:

- **macro version**: `NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_i, name, ...)` — one
  macro call inside each struct
- **reflection version**: a generic `to_json`/`from_json` over
  `nonstatic_data_members_of`; user types get **zero** declarations

`-DBENCH_N` (default 50) controls how many of the 100 defined types are
instantiated in `main()` (unused types generate no code). Both modes compile
with the SAME flags (`-std=c++26 -freflection`), and `main()` keeps the
serializers observable (their output feeds a printed counter), so `-O2` cannot
eliminate the generated code — a deliberate design choice, see the caveat below.

Reflection serializer (the one-time cost; the `namespace refl` block in the
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

**Pitfalls hit while building this (all verified GCC 16 behaviors)**:
- `std::meta::` must be qualified; `std::define_static_array` (not
  `std::meta::define_static_array`)
- `nonstatic_data_members_of` returns a **transient vector**: subscript/size
  directly on the call, never bind to a local `constexpr`
- `template for` needs the range inline or namespace-scope constexpr
- recursive serialization of nested class members does NOT fall into nlohmann's
  ADL overload set (`detail::to_json` is not a bare function); scalars must go
  through assignment / `.template get<M>()`
- `if constexpr` does NOT discard the false branch ⇒ tag-dispatch required for
  class-vs-scalar recursion

### 2.2 Results

**Q1 code saved** (per-type user lines: macro = struct + macro call = 2/type;
reflection = struct only = 1/type; one-time serializer = 25 lines):

| types | macro (lines) | reflection (lines) | net |
|---|---|---|---|
| 1 | 2 | 26 | −24 |
| 20 | 40 | 45 | −5 |
| **25** | 50 | 50 | **break-even** |
| 28 | 56 | 53 | +3 |
| 50 | 100 | 75 | +25 |
| 100 | 200 | 125 | +75 |

Reflection saves **1 line per type** but needs a **25-line one-time generic
serializer**; it only nets positive beyond ~25 types.

**Q2 compile time** (same TU, same `-std=c++26 -freflection`, min of 3):

| N | macro -O0 | refl -O0 | Δ | macro -O2 | refl -O2 | Δ |
|---|---|---|---|---|---|---|
| 1 | 2.22 s | 2.38 s | +7% | 3.11 s | 3.24 s | +4% |
| 20 | 2.43 s | 2.58 s | +6% | 3.27 s | 3.89 s | +19% |
| 28 | 2.47 s | 2.73 s | +11% | 3.38 s | 4.16 s | +23% |
| 50 | 2.64 s | 2.92 s | +11% | 3.79 s | 4.99 s | +32% |
| 100 | 3.13 s | 3.60 s | +15% | 5.13 s | 7.21 s | +41% |

**Q3 binary** (executable bytes / `size` text, per N):

| N | macro -O0 | refl -O0 | Δ exe / Δ text | macro -O2 | refl -O2 | Δ exe / Δ text |
|---|---|---|---|---|---|---|
| 1 | 393,656 / 169,213 | 484,512 / 199,086 | +23% / +18% | 113,384 / 80,111 | 119,888 / 85,244 | +6% / +6% |
| 20 | 445,936 / 202,548 | 555,608 / 260,100 | +25% / +28% | 131,088 / 94,192 | 174,456 / 129,294 | +33% / +37% |
| 28 | 470,560 / 216,580 | 584,264 / 285,788 | +24% / +32% | 145,120 / 106,241 | 194,248 / 149,994 | +34% / +41% |
| 50 | 530,088 / 255,186 | 669,208 / 356,438 | +26% / +40% | 182,608 / 138,326 | 265,248 / 207,914 | +45% / +50% |
| 100 | 667,600 / 343,012 | 850,360 / 517,124 | +27% / +51% | 266,640 / 208,925 | 421,720 / 342,974 | +58% / +64% |

**Optimization-level sensitivity (corrected story)**: the previous report
claimed the -O0 bloat "evaporates at -O2" (+90% → +12%). With an honest
benchmark that keeps the serializers observable, the -O2 gap does **not**
collapse — for small N it is smaller (+6% at N=1 vs +23% at -O0), but it
**grows with N** and ends up *larger* at -O2 (+58% exe at N=100) than at -O0
(+27%). The old collapse was an artifact of its un-checked-in TU, whose -O2
build evidently eliminated most of both serializers. Always state both the
optimization level AND whether the generated code is observable.

Bloat attribution (holds at both -O levels): the delta is **.text
(instructions), not rodata** — rodata is ~1.7 KB (-O0) / ~1.3 KB (-O2) in both
modes, so the per-member `std::string(identifier_of(m))` keys are not the cost;
the reflection path emits more per-member code (and more symbols: `nm` at N=50
counts 1,471 vs 1,621 (+10%) at -O0 and 238 vs 300 (+26%) at -O2 — text grows
faster than symbols, so it is per-instantiation code, not symbol bloat).

**Q4 diagnostics** — verified with a deliberate bad type (`std::mutex` member):

| scenario | macro/handwritten | reflection |
|---|---|---|
| member **listed** in the macro but not serializable | compile error (`no match for 'operator=' ... const std::mutex`), error points inside the macro expansion (`macro_scope.hpp:580`) | compile error, same underlying error at the serializer's `j[...] = v.[:m:]` line; both name `std::mutex` |
| member **not listed** in the macro | **silent**: `NLOHMANN_DEFINE_TYPE_INTRUSIVE(with_mutex, id)` compiles, serializes `{"id":42}` — the `mutex` member is silently dropped, round-trip is lossy, no diagnostic | impossible: the generic serializer reflects ALL members, so the mutex is a compile error |

**Interpretation (corrected)**: the old report claimed macros fail *silently at
runtime* while reflection fails at compile time for an unserializable member.
That does not reproduce: with the member listed, BOTH paths are compile errors
naming the offending type. The real asymmetry is **silent omission**: a macro
can forget a member and you get lossy serialization with zero diagnostics;
reflection cannot forget anything, so it fails loudly at compile time. That —
not error quality for listed members — is reflection's real diagnostic win.

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

# reflection vs macro (the checked-in benchmark TU, §2.2)
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
  -o /tmp/bm tests/static-reflection/bench_macro_vs_reflection.cpp      # macro
g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -DBENCH_REFLECTION -Iinclude \
  -o /tmp/br tests/static-reflection/bench_macro_vs_reflection.cpp      # refl
size /tmp/bm /tmp/br && nm /tmp/bm /tmp/br | wc -l

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
