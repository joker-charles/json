# Static-reflection fixtures after the 2026-10 core cleanup

The core headers match `develop`; the C++20 concepts layer and the refl2
catch-all/enum integration were removed from `detail::{to,from}_json`. The
reflection code is **add-on-only**:

| Entry point | Location | State |
|---|---|---|
| `<nlohmann/reflection.hpp>` | `include/nlohmann/` | supported flat-struct extension: explicit include, per-type `[[=nlohmann::json_serializable{}]]`, public-API-only |

The legacy refl2 codec and the binary-mirror headers (`reflection_to_json.hpp`,
`reflection_json.hpp`, `reflection_binary.hpp`) are **not on this branch**. They
are preserved unchanged, together with all of their fixtures and measurement
scripts, on the `legacy/reflection-experiments` branch.

## What remains here

- `reflection_flat_struct.cpp` — runtime fixture for the supported extension.
  Builds against both the split headers and the amalgamated single header.
- `compile_fail/` + `compile_fail.sh` — negative compile harness: each case
  must fail to compile with the diagnostic named on its `EXPECT-DIAG:` line.
  This is what proves the extension's rejection guards actually fire.
- `check_docs_examples.sh` — keeps `docs/mkdocs/docs/features/reflection.md`
  honest: it extracts the page's examples, compiles and runs them, and checks
  the page's per-type-opt-in and actionable-`static_assert` claims.

The doctest target for the extension lives in the main suite:
`tests/src/unit-static_reflection.cpp` (`test-static_reflection_cpp26`).

## Building

```sh
# runtime fixture
g++-16 -std=c++26 -freflection -Iinclude \
    -o build/scratch/reflection_flat_struct \
    tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct

# negative compile harness
GXX=g++-16 bash tests/static-reflection/compile_fail.sh

# documentation example check
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```

## Getting the legacy code back

```sh
git checkout legacy/reflection-experiments -- include/nlohmann/reflection_to_json.hpp
```

That branch is a frozen snapshot for reference; it is not maintained and not
part of the surface proposed upstream.
