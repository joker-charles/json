# Build recipes & toolchain

> **When to read this.** Whenever you need to configure, build, reproduce or
> measure anything on this branch — and for the one-line toolchain smoke test
> when in doubt. The non-negotiable rules around these commands live in the
> main AGENTS.md (§2 Safety boundaries); this file is the reference of HOW.
>
> **How to add a recipe.** New build lines for new fixtures go here (they also
> live in each fixture's header comment as the authoritative copy).
>
> **Current state (2026-10-29, clean-core):** the four previously modified core
> headers match `develop`; the C++20 concepts layer and the refl2 catch-all
> core integration were removed. Reflection is add-on-only. See
> `CORE_CLEANUP_2026-10.md` and `tests/static-reflection/README.md`; build
> lines that depended on the old catch-all are marked legacy below.

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
- **Add-on entry points (2026-10-29 clean-core)**: the core headers match
  `develop`; there is no catch-all in `detail::{to,from}_json`.
  - `<nlohmann/reflection.hpp>`: explicit include, no `JSON_USE_REFLECTION`;
    per-type `[[=nlohmann::json_serializable{}]]`; inert without P2996.
  - `<nlohmann/reflection_to_json.hpp>`: legacy refl2 codec, explicit include
    after `json.hpp` with `JSON_USE_REFLECTION`; invoke
    `refl2::codec<false>::to_json/from_json` explicitly.
  - `<nlohmann/reflection_binary.hpp>` / `reflection_json.hpp`: legacy binary
    mirror, explicit include.

## Fixture build lines

Reference for the artifacts (all under `g++-16`):

```sh
# M1 enum reflection + probes
g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude -o /tmp/p repro_m1.cpp
# M2/M3 differential (vs the real library) + ASan leak check
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
# [removed 2026-10] C++20 concepts smoke test: the concepts layer is deleted.
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
# Standalone flat-struct reflection extension (explicit add-on; no JSON_USE_REFLECTION)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o build/scratch/reflection_flat_struct \
  tests/static-reflection/reflection_flat_struct.cpp && ./build/scratch/reflection_flat_struct
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
  -o build/scratch/reflection_flat_struct_single \
  tests/static-reflection/reflection_flat_struct.cpp && ./build/scratch/reflection_flat_struct_single
# inert without P2996 (must compile; nlohmann::json_serializable is not declared)
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
  g++-16 -std=c++11 -Iinclude -x c++ - -fsyntax-only
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
  g++-16 -std=c++26 -Iinclude -x c++ - -fsyntax-only
```

The M5/M6/M7/type-optin build lines above are historical: they depended on
the removed core catch-all and are excluded from the clean-core build. See
`tests/static-reflection/README.md`. The flat-struct line and the binary-mirror
lines are current.

The full fixture list lives in `tests/static-reflection/`; each file documents
its own build line in its header comment. The main-library changes are also
covered by the repository's doctest unit suite (see "Repository test suite"
below).

The standalone flat-struct extension (`include/nlohmann/reflection.hpp`) is
**not** in the `json.hpp` include chain, so it is not amalgamated into
`single_include/nlohmann/json.hpp`; do not add it to
`tools/amalgamate/config_json.json`. The split add-on works with the
amalgamated library via `-Isingle_include -Iinclude`.

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
  test-conversions_cpp17 -j2
./build-test/tests/test-udt_cpp11
./build-test/tests/test-serialization_cpp11
./build-test/tests/test-conversions_cpp17
```

`tests/CMakeLists.txt` matches `develop`: there is no C++26 reflection doctest
target and no concepts dual-path target in the clean-core tree.

### Static-reflection add-on fixtures

Reflection tests are standalone add-on fixtures, not CTest targets:
`tests/static-reflection/reflection_flat_struct.cpp` (the supported extension),
explicit `refl2::codec` probes, binary-mirror fixtures, and the compile-fail
harness (`GXX=g++-16 bash tests/static-reflection/compile_fail.sh`). See
`tests/static-reflection/README.md` and `CORE_CLEANUP_2026-10.md` for the
supported/legacy split.

### Documentation example check (`check_docs_examples.sh`)

> **Legacy (pre-2026-10):** this compiles the old `probe_json_schema` and the
> old mkdocs reflection page, which describe the removed catch-all; it is not
> part of the clean-core build.

`docs/mkdocs/docs/features/reflection.md` contains two fenced blocks that are
executable claims: the `namespace schema` JSON Schema generator (```cpp) and
the JSON it produces for `person` (```json). Nothing compiled them, so the page
could drift from the library silently.

```sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```

It (1) compiles and runs `tests/static-reflection/probe_json_schema.cpp`, whose
own cross-check asserts the schema's keys equal what the codec emits; (2)
extracts the page's ```cpp block **verbatim**, wraps it in the fixtures the page
shows, and compiles it; (3) compares its output against the page's ```json
block by parsing both, so key order does not matter. Exits non-zero on any of
those. Verified to fail on all three drift modes (tampered output block,
tampered code block, broken fixture).

Pairs with the compile-fail harness:

```sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh   # all auto-discovered cases, must fail to compile
```
