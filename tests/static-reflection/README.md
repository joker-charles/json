# Static-reflection fixtures

Everything here belongs to `<nlohmann/reflection.hpp>` — the only reflection
code on this branch. Its contract is `docs/static-reflection/EXTENSION.md`; build
commands are in `docs/static-reflection/README.md`.

| File | Purpose |
|---|---|
| `reflection_flat_struct.cpp` | runtime fixture; builds against split headers and the amalgamated single header |
| `compile_fail/` + `compile_fail.sh` | negative cases: each must fail to compile with the diagnostic on its `EXPECT-DIAG:` line |
| `check_docs_examples.sh` | compiles the mkdocs page's examples and checks its claims |

The doctest target lives in the main suite:
`tests/src/unit-static_reflection.cpp` (`test-static_reflection_cpp26`).

```sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```

The retired refl2 codec, binary mirror and their ~45 fixtures are on the
`legacy/reflection-experiments` branch, not here.
