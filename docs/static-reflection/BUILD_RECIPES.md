# Build recipes & toolchain

> **When to read this.** Whenever you need to configure, build, reproduce or
> measure anything on this branch — and for the one-line toolchain smoke test
> when in doubt. The non-negotiable rules around these commands live in the
> main AGENTS.md (§2 Safety boundaries); this file is the reference of HOW.
>
> **How to add a recipe.** New build lines for new fixtures go here (they also
> live in each fixture's header comment as the authoritative copy).

## Toolchain — trust, don't re-derive

- Use **`g++-16`** (GCC 16.1.0). The system default `g++` is 15 and **has no
  `<meta>`** (nor other C++26 headers) — reflection code will fail with
  `fatal error: meta: No such file or directory` if compiled with the wrong
  compiler.
- Reflection build flags: `-std=c++26 -freflection`.
- Smoke test (one line, cheap confidence — not a full re-verification):
  ```sh
  printf 'int main(){}' | g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t && echo "g++-16 OK"
  ```
- CMake presets (`CMakeUserPresets.json`, gitignored): `cxx26-reflect` /
  `cxx26-reflect-tests` pin `g++-16` + `CMAKE_CXX_STANDARD=26` (+
  `JSON_TestStandards=26` for the suite). **Do NOT put `-freflection` into
  `CMAKE_CXX_FLAGS`** — it leaks into the nested-project cmake tests
  (`tests/cmake_add_subdirectory` etc.), whose `TryCompile` probes compile with
  the *default* standard and die with `'-freflection' only supported with
  '-std=c++26'` (this exact failure broke 13 ctest cases until the preset was
  fixed). Reflection is consumer-side: add `-freflection` only to the TUs that
  actually use P2996 (the reflection fixtures do this via their own build
  lines).
- Two include layouts: split headers under `-Iinclude`, or the amalgamated
  single header under `-Isingle_include`. Reflection/concepts work targets the
  split `-Iinclude` layout (the `#ifdef JSON_HAS_CPP_20` branches only compile
  there).
- **Per-type opt-in gate (`JSON_USE_REFLECTION`, UPSTREAM_INTEGRATION_PLAN.md
  §2.2)**: since the reflection catch-all is a behavior change, it compiles
  only when the user defines `JSON_USE_REFLECTION` (the compiler must still
  provide P2996: g++-16 `-std=c++26 -freflection`). Both the include chain
  (`{to,from}_json.hpp`) and `reflection_to_json.hpp` itself enforce it — a
  direct include without the macro fails with a clear `#error`. Every
  reflection fixture defines the macro at the top of its file (before any
  `#include`), so the build lines below need no extra `-D`; additionally,
  each struct expected to serialize through the catch-all carries the
  type-level annotation `[[=refl2::json_serializable{}]]` (annotation AFTER
  the `struct` keyword; unannotated reflectable structs keep the main
  library's behavior — compile error).

## Fixture build lines

Reference for the artifacts (all under `g++-16`):

```sh
# M1 enum reflection + probes
g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude -o /tmp/p repro_m1.cpp
# M2/M3 differential (vs the real library) + ASan leak check
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
# M4 concepts dual-path smoke (compile for -std=c++11/20/26; outputs must match)
g++-16 -std=c++20 -O0 -Iinclude -o /tmp/s tests/static-reflection/concepts_smoke.cpp && /tmp/s
# M5 macro<->annotation differential + zero drift (reflection vs baseline)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/prm_r tests/static-reflection/probe_reflection_replace_macros.cpp && /tmp/prm_r
g++-16 -std=c++26 -O1 -Iinclude \
  -o /tmp/prm_b tests/static-reflection/probe_reflection_replace_macros.cpp && /tmp/prm_b
#   diff <(grep '^COMMON' <(./prm_b)) <(grep '^COMMON' <(./prm_r)) must be empty
# M6 enum string mapping (macro<->annotation + zero drift)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/per_r tests/static-reflection/probe_enum_reflection.cpp && /tmp/per_r
g++-16 -std=c++26 -O1 -Iinclude \
  -o /tmp/per_b tests/static-reflection/probe_enum_reflection.cpp && /tmp/per_b
# M4B binary byte-code tables: msgpack/ubjson(no-opt)/bson differential + ASan
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/m4b tests/static-reflection/m4_binary.cpp && /tmp/m4b
# M4B-2 BSON reader: read differential vs from_bson + negatives + ASan
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/m4b2 tests/static-reflection/m4b_bson_reader.cpp && /tmp/m4b2
# M4D API surface: type_name/at/erase/clear/swap/compare + value_t tables
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/m4d tests/static-reflection/m4d_api.cpp && /tmp/m4d
# M4E UBJSON optimized modes + BJData (draft2/draft3)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/m4e tests/static-reflection/m4e_ubjson_opt.cpp && /tmp/m4e
# M7 top-level std::variant (codec)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/m7 tests/static-reflection/probe_variant.cpp && /tmp/m7
# M4D-2 iterators: begin/end/operator[]/find/erase(iterator)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o /tmp/m4d2 tests/static-reflection/m4d2_iterators.cpp && /tmp/m4d2
```

The full fixture list lives in `tests/static-reflection/`; each file documents
its own build line in its header comment. The main-library changes are also
covered by the repository's doctest unit suite (see "Repository test suite"
below).

## Single-header regeneration (amalgamate)

`single_include/nlohmann/json.hpp` is an amalgamated snapshot of the split
headers — never edit it directly. After touching a split header read into the
single header, re-amalgamate so it stays in sync:

```sh
make amalgamate   # amalganmate.py + `make pretty` (astyle 3.4.13 via tools/astyle/venv)
```

astyle 3.4.13 (the CI-pinned version) and the venv are set up in this
workspace (`tools/astyle/venv`, gitignored), so `make amalgamate` and `make
pretty` are fully reproducible locally. If you only need the single header
regenerated without the astyle pass, run:

```sh
python3 tools/amalgamate/amalgamate.py -c tools/amalgamate/config_json.json -s .
```

Note: `make pretty` formats **all** `include/`, `tests/`, and `docs/examples/`
sources; review the diff and keep unrelated pre-existing files out of the
commit (see da68ff71 for the cleanup of docs/examples/parser_callback_t.cpp).

## Benchmark and evaluation reproduction

The commands EVALUATION.md §4 used to inline, moved here as the authoritative
copy (baseline worktree, the three-mode benchmark sweep, runtime throughput,
and the evaluation probes). Each probe's header comment remains the
authoritative build line for that file.

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

## Repository test suite

The main-library changes are exercised by the repository's doctest suite.
Configure and build a subset, e.g.:

```sh
cmake -S . -B build-test -DCMAKE_CXX_COMPILER=g++-16 \
  -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
cmake --build build-test --target test-udt_cpp11 test-serialization_cpp11 \
  test-conversions_cpp17 test-concepts_dual_cpp20 -j2
./build-test/tests/test-concepts_dual_cpp20
```

`tests/src/unit-concepts_dual.cpp` (mentions `JSON_HAS_CPP_20`) registers
both `_cpp11` and `_cpp20` targets and proves the dual path is behavior-preserving.

### Static-reflection doctest target (`test-static_reflection_cpp26`)

`tests/src/unit-static_reflection.cpp` ports the opt-in gate coverage into the
upstream doctest suite (per-type `[[=refl2::json_serializable{}]]` annotation,
byte-identical macro-path comparison, M6 enum mapping, M7 variant whitelist).
It is C++26-only (P2996):

- `tests/CMakeLists.txt` excludes it from the default-standard glob (no
  C++11/14/17/20/23 targets) and registers it explicitly with
  `CXX_STANDARDS 26`; `-freflection` is added **only** to that target via
  `json_test_set_test_options` (never globally).
- `compiler_supports_cpp_26` is defined only for GCC ≥ 16, so default builds
  on other compilers never see the target. On g++-16 it is built and run with
  the ordinary suite:
  ```sh
  cmake --build build-test --target test-static_reflection_cpp26 -j2
  ./build-test/tests/test-static_reflection_cpp26
  ```
- For a full C++26 sweep of the whole doctest suite, configure with
  `-DJSON_TestStandards=26` (the `cxx26-reflect-tests` preset).
