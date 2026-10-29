# docs/static-reflection — index

Branch-internal documentation for the static-reflection work on
`feature/static-reflection`. This directory is **not** part of the published
website (that is `docs/mkdocs/docs/features/reflection.md`).

The branch has one supported deliverable: `<nlohmann/reflection.hpp>`, an
explicit, per-type-opt-in flat-struct serializer built only on the public
`basic_json` interface.

## What to read

| You want | Read |
|---|---|
| The extension's contract, scope and test matrix | [`FLAT_STRUCT_REFLECTION.md`](FLAT_STRUCT_REFLECTION.md) |
| What to propose upstream, and under which constraints | [`UPSTREAM_INTEGRATION_PLAN.md`](UPSTREAM_INTEGRATION_PLAN.md) |
| How the branch got to its current shape | [`CORE_CLEANUP_2026-10.md`](CORE_CLEANUP_2026-10.md) |
| The planned next slice (private-member access) | [`PRIVATE_MEMBER_REFLECTION_PLAN.md`](PRIVATE_MEMBER_REFLECTION_PLAN.md) |
| Verified compiler/reflection facts (don't re-derive) | [`VERIFIED_FACTS.md`](VERIFIED_FACTS.md) |
| Build, test, amalgamate commands | [`BUILD_RECIPES.md`](BUILD_RECIPES.md) |

## What is no longer here

The branch previously carried a much larger exploration: a wide-open `refl2`
catch-all wired into `detail/{to,from}_json`, an annotation layer
(`json_name`/`json_ignore`/`json_default`), enum string mapping, reflection-driven
MessagePack/UBJSON/BSON/CBOR/BJData readers and writers, and a
reflection-backed `basic_json_reflection` reimplementation of the tagged union.
None of it is part of the proposed upstream surface, and all of it reached
`basic_json`'s private layout through `access_context::unchecked()`.

That work — code, milestone reports, measurements and defect reproductions —
is preserved on the **`legacy/reflection-experiments`** branch. Do not
reintroduce it here; the first PR is deliberately one header and one test file.

Toolchain hard requirement for anything reflection-related: `g++-16 -std=c++26
-freflection` (see `BUILD_RECIPES.md`).
