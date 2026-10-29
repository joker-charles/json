# Static-reflection fixtures after the 2026-10 core cleanup

The core headers now match `develop`; the C++20 concepts layer and the refl2
catch-all/enum integration were removed from `detail::{to,from}_json`. The
reflection code is **add-on-only**:

| Entry point | State |
|---|---|
| `<nlohmann/reflection.hpp>` | supported flat-struct extension: explicit include, per-type `[[=nlohmann::json_serializable{}]]`, public-API-only |
| `<nlohmann/reflection_to_json.hpp>` | legacy refl2 codec: explicit add-on only, requires `JSON_USE_REFLECTION`, use `refl2::codec<false>::to_json/from_json` directly |
| `<nlohmann/reflection_binary.hpp>` / `reflection_json.hpp` | legacy binary mirror: explicit add-on, independent of the core catch-all |

## Supported clean-core fixtures

- `reflection_flat_struct.cpp` — the new flat-struct extension (split and
  amalgamated-library layouts).
- `probe_coverage_boundary.cpp`, `probe_type_alignment.cpp` — explicit
  `refl2::codec<false>` coverage, no core catch-all.
- `m2_diff.cpp`, `m3_binary.cpp`, `m4_binary.cpp`, `m4b_bson_reader.cpp`,
  `m4d_api.cpp`, `m4e_ubjson_opt.cpp`, `m4f_binary_readers.cpp`,
  `m4d2_iterators.cpp`, `probe_real_json_value.cpp`, `regression_fixes.cpp` —
  explicit binary/mirror add-on fixtures.
- `compile_fail/*.cpp` — the harness auto-discovers all cases; the clean-core
  set passes with `GXX=g++-16 bash compile_fail.sh`. The legacy refl2 cases
  that call `refl2::codec` directly still work; cases that merely expect an
  unannotated type not to serialize still fail for the baseline reason.

## Legacy automatic-integration fixtures (excluded)

The following fixtures were written for the removed `detail::{to,from}_json`
catch-all (they use `json j = annotated;` / `j.get<T>()` without an explicit
codec call). They are kept for historical reference and are **not** part of
the clean-core build:

- `probe_adl_recursion.cpp`
- `probe_reflection_replace_macros.cpp`
- `probe_json_schema.cpp`
- `probe_enum_reflection.cpp`
- `probe_variant.cpp`
- `probe_negative_runtime.cpp`
- `type_optin_diff.cpp`
- `probe_complex_diff.cpp`
- `bench_macro_vs_reflection.cpp` and the benchmark drivers that assume the
  catch-all
- `check_docs_examples.sh` (it compiles the old `probe_json_schema` and the
  old mkdocs reflection page)

They can be revived only by reintroducing a core integration hook or by
migrating them to `adl_serializer<T, void>`; the branch intentionally does
not do that in the clean-core state.

## Quick verification

```sh
# supported flat-struct extension
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o build/scratch/reflection_flat_struct \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct

# single-header library + split add-on
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
  -Isingle_include -Iinclude -o build/scratch/reflection_flat_struct_single \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct_single

# negative cases (all auto-discovered)
GXX=g++-16 bash tests/static-reflection/compile_fail.sh

# explicit legacy codec (add JSON_USE_REFLECTION and include the header)
g++-16 -std=c++26 -freflection -O1 -Iinclude \
  -o build/scratch/probe_coverage_boundary \
  tests/static-reflection/probe_coverage_boundary.cpp
./build/scratch/probe_coverage_boundary
```
