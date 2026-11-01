# Static-reflection fixtures

Fixtures and harnesses for `<nlohmann/reflection.hpp>` and, on the
`reflection-opt-in-compare` experiment branch,
`<nlohmann/reflection_namespace.hpp>`.

| File | Purpose |
|---|---|
| `reflection_flat_struct.cpp` | annotation runtime fixture; builds against split headers and the amalgamated single header |
| `reflection_namespace.cpp` | namespace-route fixture: annotation, `enable<T>` without `using`, third-party type, custom ADL precedence, nested/container members, `ordered_json`, error semantics, direct qualified calls |
| `optin_conflicts.cpp` | precedence matrix: annotation / `enable<T>` / `using` × custom ADL / `NLOHMANN_DEFINE_TYPE_*` / explicit `adl_serializer` |
| `optin_namespace_wide.cpp` | `JSON_REFLECTION_NAMESPACE_UNCONSTRAINED` mode: `using` alone opts in; custom and macro precedence |
| `optin_odr/` | two TUs where only one sees `enable<T>`; both compile but disagree on `is_constructible` (ODR/visibility hazard) |
| `compile_fail/` + `compile_fail.sh` | annotation negative cases; each must fail with the diagnostic on its `EXPECT-DIAG:` line; also the P2996 `#error` |
| `namespace_compile_fail/` + `namespace_compile_fail.sh` | namespace negative cases: using-only is not an opt-in, enable-after-use, private/base/union/unsupported members, nested type not enabled, unconstrained hard error, generic ambiguity |
| `optin_matrix.sh` | runs all of the above, both include layouts, plus the annotation baseline |
| `measure_optin.sh` | generates N annotation/enable types, compares `-O2` object text/data/bss (expected identical) |
| `check_docs_examples.sh` | compiles the mkdocs page's examples and checks its claims |

The doctest target lives in the main suite:
`tests/src/unit-static_reflection.cpp` (`test-static_reflection_cpp26`), which
gets `-DJSON_USE_REFLECTION` from `tests/CMakeLists.txt`. The runtime fixtures
define the macro in-source; the harnesses pass it per compile.

```sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
GXX=g++-16 bash tests/static-reflection/namespace_compile_fail.sh
GXX=g++-16 bash tests/static-reflection/optin_matrix.sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```
