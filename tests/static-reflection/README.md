# Static-reflection fixtures

Fixtures and harnesses for `<nlohmann/reflection.hpp>`.

| File | Purpose |
|---|---|
| `reflection_flat_struct.cpp` | runtime fixture; builds against split headers and the amalgamated single header |
| `compile_fail/` + `compile_fail.sh` | negative cases: each must fail to compile with the diagnostic on its `EXPECT-DIAG:` line; also checks the `JSON_USE_REFLECTION`-without-P2996 `#error` |
| `check_docs_examples.sh` | compiles the mkdocs page's examples and checks its claims |

The doctest target lives in the main suite:
`tests/src/unit-static_reflection.cpp` (`test-static_reflection_cpp26`), which
gets `-DJSON_USE_REFLECTION` from `tests/CMakeLists.txt`. The runtime fixture
defines the macro in-source; the two harness scripts pass it per compile.

```sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```
