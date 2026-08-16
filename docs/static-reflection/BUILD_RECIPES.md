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
