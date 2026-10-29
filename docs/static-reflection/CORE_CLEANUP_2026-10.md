# 2026-10 core cleanup: reflection is add-on-only

## Contract

The four previously modified core headers are back to the `develop` baseline:

- `include/nlohmann/detail/input/lexer.hpp`
- `include/nlohmann/detail/input/binary_reader.hpp`
- `include/nlohmann/detail/conversions/to_json.hpp`
- `include/nlohmann/detail/conversions/from_json.hpp`

`git diff develop --` over the whole `include/nlohmann` tree now shows **only
new add-on headers** (`reflection.hpp`, `reflection_binary.hpp`,
`reflection_json.hpp`, `reflection_to_json.hpp`) — no modified core file.
`single_include/nlohmann/json.hpp` is regenerated and matches `develop`; the
new add-on is not amalgamated.

The C++20 concepts layer is deleted:

- `include/nlohmann/detail/concepts/concepts.hpp` removed;
- `to_json.hpp` / `from_json.hpp` use their `develop` enable_if/SFINAE paths;
- concept-specific tests (`unit-concepts_dual.cpp`, `concepts_smoke.cpp`,
  `probe_layered_concepts.cpp`, `probe_composite_concept.cpp`,
  `probe_draft_drift.cpp`) removed;
- `tests/CMakeLists.txt` reverted to `develop` (no C++26 reflection targets,
  no gate-on regression targets).

The lexer/binary_reader `std::from_chars`/hex error-message changes were not
reflection features; they were reverted with the rest of the core so the tree
is reviewable as "develop + add-ons".

## Current add-on layout

| Entry point | Status | How it is used |
|---|---|---|
| `<nlohmann/reflection.hpp>` | supported | explicit include; per-type `[[=nlohmann::json_serializable{}]]`; flat public members through the public `basic_json` API |
| `<nlohmann/reflection_to_json.hpp>` | legacy | explicit include after `json.hpp`, requires `JSON_USE_REFLECTION`; call `refl2::codec<false>::to_json/from_json` directly |
| `<nlohmann/reflection_binary.hpp>` / `reflection_json.hpp` | legacy | explicit binary/mirror add-on |

The old refl2 catch-all and enum overloads are no longer injected into
`detail::{to,from}_json`. `reflection_to_json.hpp` had its dead catch-all
definition and `JSON_HAS_CPP_26_REFLECTION` macro removed; it now uses a local
`REFL2_THROW` fallback because a direct include after `json.hpp` no longer has
the library's `JSON_THROW` macro (it was undefined by `macro_unscope.hpp`).

## Tests

- Main doctest suite: unchanged upstream targets; `test-udt_cpp11`,
  `test-serialization_cpp11`, `test-conversions_cpp17` pass.
- Supported reflection fixtures:
  - `tests/static-reflection/reflection_flat_struct.cpp` (split and
    `-Isingle_include -Iinclude`);
  - `tests/static-reflection/compile_fail/*` (19/19, including the new
    flat-struct guards);
  - explicit legacy codec probes (`probe_coverage_boundary.cpp`,
    `probe_type_alignment.cpp`);
  - explicit binary-mirror fixtures (`m2_diff.cpp`, `m3_binary.cpp`,
    `m4_binary.cpp`, `m4b_bson_reader.cpp`, `m4d_api.cpp`,
    `m4e_ubjson_opt.cpp`, `m4f_binary_readers.cpp`, `m4d2_iterators.cpp`).
- Legacy automatic-integration fixtures (old `json j = annotated;` /
  `j.get<T>()` behavior) are excluded and listed in
  `tests/static-reflection/README.md`.

## CI note

`cmake/ci.cmake` excludes the legacy C++26-only add-on headers from the C++11
per-header self-containment check. The new `<nlohmann/reflection.hpp>` is
inert without P2996, so it stays in the check and compiles under C++11.

## Verification

```sh
# no core modifications: only A entries for the reflection add-ons
git diff --name-status develop -- include/nlohmann

# single header is develop-clean
git diff --stat develop -- single_include

# no concepts references left
! grep -RIn 'detail/concepts/concepts.hpp' include cmake tests/CMakeLists.txt

# supported fixture
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o build/scratch/reflection_flat_struct \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct
GXX=g++-16 bash tests/static-reflection/compile_fail.sh

# main suite (network-free subset)
cmake -S . -B build-test-clean -DCMAKE_CXX_COMPILER=g++-16 \
  -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
cmake --build build-test-clean --target test-udt_cpp11 \
  test-serialization_cpp11 test-conversions_cpp17 -j2
```

The full `ci_single_binaries` target could not be built in this sandbox
because `include-what-you-use` is not installed. The filtered source list was
checked by hand: `<nlohmann/reflection.hpp>` compiles under C++11 (it is
inert), and the three legacy C++26-only add-on headers are excluded.

## Follow-ups (not in the cleanup)

- If the legacy refl2 codec is still needed for new code, migrate it to a
  constrained `adl_serializer<T, void>` partial specialization (public
  customization point) instead of restoring a core catch-all.
- The legacy automatic-integration fixtures can be adapted to explicit
  `refl2::codec` calls or removed once their milestones are archived.
- The old mkdocs reflection page and `check_docs_examples.sh` describe the
  pre-cleanup catch-all and are not part of the clean-core build.
