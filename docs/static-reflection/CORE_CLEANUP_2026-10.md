# 2026-10 cleanup: reflection is add-on-only

Record of how the branch reached its current shape. Two stages, both complete.

## Stage 1 — restore the core

The four core headers that earlier work had modified are back to the `develop`
baseline, byte-identical:

- `include/nlohmann/detail/input/lexer.hpp`
- `include/nlohmann/detail/input/binary_reader.hpp`
- `include/nlohmann/detail/conversions/to_json.hpp`
- `include/nlohmann/detail/conversions/from_json.hpp`

The C++20 concepts layer was removed with them, and the `refl2` catch-all/enum
overloads are no longer injected into `detail::{to,from}_json`.
`single_include/nlohmann/json.hpp` was regenerated and matches `develop`.

Consequence: the C++11–C++26 contract is untouched, and the tree is reviewable
as "develop + add-ons".

## Stage 2 — park the legacy code

The remaining reflection code under `include/nlohmann/` was an exploration, not
a candidate for upstream:

| Header | Lines | Why it is out |
|---|---|---|
| `reflection_to_json.hpp` | 1545 | wide-open catch-all + `refl2` codec; reaches private `json_value` via `unchecked()` |
| `reflection_json.hpp` | 7411 | standalone binary/tagged-union playground; same private-layout dependency |
| `reflection_binary.hpp` | 21 | facade for the above |

All three were moved off this branch, together with their 46 fixtures, benches,
measurement snapshots and defect reproductions, to the
**`legacy/reflection-experiments`** branch (pushed to the fork; frozen at the
last state where the codec was still wired into the core). They were never part
of `json.hpp`, never referenced by the build, and never covered by a CMake
target, so removing them changed no build.

The mkdocs pages `JSON_USE_REFLECTION` / `JSON_HAS_CPP_26_REFLECTION` described
that codec and have been removed as well.

## Current layout

| Entry point | Status |
|---|---|
| `<nlohmann/reflection.hpp>` | the only reflection header; explicit include, per-type `[[=nlohmann::json_serializable{}]]`, public `basic_json` API only |

## Tests after both stages

- Main doctest suite: unchanged upstream targets; `test-udt_cpp11`,
  `test-serialization_cpp11`, `test-conversions_cpp17` pass.
- `tests/src/unit-static_reflection.cpp` / `test-static_reflection_cpp26`:
  C++26 doctest for the extension (4 cases / 35 assertions).
- `tests/static-reflection/`:
  - `reflection_flat_struct.cpp` — runtime fixture (split and
    `-Isingle_include -Iinclude` layouts);
  - `compile_fail/` + `compile_fail.sh` — 7 extension-specific negative cases;
  - `check_docs_examples.sh` — keeps the mkdocs page's claims executable.

## CI note

`cmake/ci.cmake` no longer needs an exclusion for legacy C++26-only add-on
headers: `<nlohmann/reflection.hpp>` is inert without P2996 and compiles under
C++11, so it stays in the per-header self-containment check.

## Verification

```sh
# only the one add-on differs from develop under include/
git diff --name-status develop -- include/nlohmann
#   A include/nlohmann/reflection.hpp

# single header is develop-clean
git diff --stat develop -- single_include

# no concepts references left
! grep -RIn 'detail/concepts/concepts.hpp' include cmake tests/CMakeLists.txt

# extension fixtures and harnesses
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o build/scratch/reflection_flat_struct \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh

# main suite (network-free subset)
cmake -S . -B build-test-clean -DCMAKE_CXX_COMPILER=g++-16 \
  -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
cmake --build build-test-clean --target test-udt_cpp11 \
  test-serialization_cpp11 test-conversions_cpp17 -j2
cmake --build build-test-clean --target test-static_reflection_cpp26 -j2
./build-test-clean/tests/test-static_reflection_cpp26
```

## Follow-ups (not in this cleanup)

- Adapt the retired codec's ideas that are still wanted (member rename/skip,
  enum mapping, binary formats) as later slices on top of the public-API-only
  design — one reviewable piece at a time.
- Broader CI wiring: a GCC 16 job dedicated to the extension.
