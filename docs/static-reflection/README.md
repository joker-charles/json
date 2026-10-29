# docs/static-reflection — index

Branch-internal documentation for `feature/static-reflection`. Not published on
the website; the user-facing page is
`docs/mkdocs/docs/features/reflection.md`.

The branch ships **one** thing: `include/nlohmann/reflection.hpp`, an explicit
add-on behind `JSON_USE_REFLECTION` that serializes annotated flat structs
through the public `basic_json` API. Nothing else under `include/` differs from
`develop`.

## The three documents

| File | Read it for |
|---|---|
| [`EXTENSION.md`](EXTENSION.md) | the contract: opt-in, what serializes, what is rejected and with which diagnostic, semantics, non-goals |
| [`VERIFIED_FACTS.md`](VERIFIED_FACTS.md) | verified toolchain and P2996 facts — do not re-derive |
| this file | commands, upstream plan, history |

## Build and test

Requirements: `g++-16` with `-std=c++26 -freflection` (the default `g++` has no
`<meta>`). Never put `-freflection` into `CMAKE_CXX_FLAGS`: it leaks into the
nested cmake tests and breaks their `TryCompile` probes.

```sh
# toolchain smoke
printf 'int main(){}' | g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t

# runtime fixture (split headers, then the amalgamated library)
g++-16 -std=c++26 -freflection -Iinclude -o /tmp/rfs \
    tests/static-reflection/reflection_flat_struct.cpp && /tmp/rfs
g++-16 -std=c++26 -freflection -Isingle_include -Iinclude -o /tmp/rfs2 \
    tests/static-reflection/reflection_flat_struct.cpp && /tmp/rfs2

# negative compile harness and documentation check
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh

# doctest target
cmake -S . -B build-test -DCMAKE_CXX_COMPILER=g++-16 \
  -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
cmake --build build-test --target test-static_reflection_cpp26 -j2
./build-test/tests/test-static_reflection_cpp26
```

The header is a no-op unless `JSON_USE_REFLECTION` is defined (so it compiles
under `-std=c++11` … `-std=c++26`), and it is **not** amalgamated: do not add it
to `tools/amalgamate/config_json.json`. The doctest target gets
`-DJSON_USE_REFLECTION` from `tests/CMakeLists.txt`, the two harness scripts pass
it per compile, and the runtime fixture defines it in-source.

## Proposing it upstream

The maintainer agreed to consider an opt-in, C++26-only extension, under these
conditions — they are the acceptance criteria for the branch:

1. a separate header, never included by `json.hpp` or `single_include`;
2. per-type opt-in only (`struct [[=nlohmann::json_serializable{}]] S`),
   unannotated types unchanged;
3. public API only — no reflection into `basic_json`'s private members;
4. no core-header changes in the same effort, and no binary formats;
5. small first PR — nested types via the normal ADL path plus the actionable
   `static_assert`; annotation features later;
6. experimental until a second compiler implements P2996 (only GCC 16 does).

Everything on this branch is scoped to 1–5. What PR 1 contains:
`include/nlohmann/reflection.hpp`, `tests/src/unit-static_reflection.cpp`, the
fixtures/harnesses under `tests/static-reflection/`, the feature page, one
`tests/CMakeLists.txt` target, one nav entry and one GCC 16 CI job. No
core-header or single-header change.

The surface proposed upstream (issue
[#5959](https://github.com/nlohmann/json/issues/5959), following the
maintainer's reply in
[#5384](https://github.com/nlohmann/json/discussions/5384)):

- header name: `<nlohmann/reflection.hpp>`;
- annotation name: `nlohmann::json_serializable`, used as
  `struct [[=nlohmann::json_serializable{}]] S`;
- macro name: `JSON_USE_REFLECTION`, undefined by default; a presence switch,
  so any definition enables it; defining it without P2996 is a `#error`, never a
  silent no-op;
- namespace: `nlohmann`, as for the rest of the public API.

## History

Earlier, this branch explored much more: a wide-open catch-all wired into
`detail/{to,from}_json`, a `refl2` codec with an annotation layer, enum string
mapping, reflection-driven MessagePack/UBJSON/BSON/CBOR/BJData codecs, and a
reflection-backed reimplementation of the tagged union. All of it reached
`basic_json`'s private `json_value` layout through
`access_context::unchecked()` and changed behavior for types that never opted
in, so it was retired: the four core headers went back to `develop`, the
concepts layer and the add-on codecs were removed.

That work — code, milestone reports, benchmarks, measurements and defect
reproductions — is preserved on the **`legacy/reflection-experiments`** branch.
Do not reintroduce it here.
