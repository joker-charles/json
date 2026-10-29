# Build recipes & toolchain

> **When to read this.** Whenever you need to build, run or reproduce anything
> on this branch. The non-negotiable rules live in the root `AGENTS.md` (§2);
> this file is the reference of *how*.

## Toolchain — trust, don't re-derive

- Use **`g++-16`**. The system default `g++` is 15 and has no `<meta>`:
  reflection code fails with `fatal error: meta: No such file or directory`.
- Reflection flags: `-std=c++26 -freflection`. `-freflection` is what defines
  `__cpp_impl_reflection` / `__cpp_lib_reflection`; plain `-std=c++26` defines
  neither.
- Smoke test:
  ```sh
  printf 'int main(){}' | g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t && echo "g++-16 OK"
  ```
- **Do NOT put `-freflection` into `CMAKE_CXX_FLAGS`**: it leaks into the
  nested-project cmake tests, whose `TryCompile` probes use the default standard
  and die with `'-freflection' only supported with '-std=c++26'`.
  Reflection is consumer-side: only TUs that actually use P2996 get the flag.

## The one supported entry point

```cpp
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>   // not in json.hpp, not amalgamated
```

`<nlohmann/reflection.hpp>` is inert on non-P2996 toolchains: including it
compiles, but `nlohmann::json_serializable` is not declared, so annotated code
fails at the annotation. It must **not** be added to
`tools/amalgamate/config_json.json`.

## Fixture build lines

```sh
# runtime fixture, split-header layout
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
    -o build/scratch/reflection_flat_struct \
    tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct

# same fixture against the amalgamated single header
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Isingle_include -Iinclude \
    -o build/scratch/reflection_flat_struct_single \
    tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct_single

# the header must stay inert (compiles, nothing declared)
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
    g++-16 -std=c++11 -Iinclude -x c++ - -fsyntax-only
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
    g++-16 -std=c++26 -Iinclude -x c++ - -fsyntax-only
```

## Harnesses

```sh
# negative compile harness: every case must fail with its EXPECT-DIAG message
GXX=g++-16 bash tests/static-reflection/compile_fail.sh

# docs: the mkdocs page's examples must compile, run and print what it claims
GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
```

Both are auto-discovering: adding a `compile_fail/*.cpp` with an `EXPECT-DIAG:`
line is enough to register a new negative case.

## Repository test suite

`tests/CMakeLists.txt` is `develop` plus one C++26 target,
`test-static_reflection_cpp26`, which compiles
`tests/src/unit-static_reflection.cpp` with `-freflection`:

```sh
cmake -S . -B build-test -DCMAKE_CXX_COMPILER=g++-16 \
  -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
cmake --build build-test --target test-static_reflection_cpp26 -j2
./build-test/tests/test-static_reflection_cpp26

# the rest of the suite must stay green (C++11 contract untouched)
cmake --build build-test --target test-udt_cpp11 test-serialization_cpp11 \
  test-conversions_cpp17 -j2
./build-test/tests/test-udt_cpp11
./build-test/tests/test-serialization_cpp11
./build-test/tests/test-conversions_cpp17
```

## Single-header regeneration (amalgamate)

`single_include/nlohmann/json.hpp` is a build artifact — never edit it by hand.
The reflection add-on is not amalgamated, so this branch needs no regeneration
unless a split header that *is* read into the single header changes (none
should; the core headers match `develop`).

```sh
make amalgamate   # amalgamate.py + astyle 3.4.13 (tools/astyle/venv, gitignored)
# or, without the astyle pass:
python3 tools/amalgamate/amalgamate.py -c tools/amalgamate/config_json.json -s .
```

`make pretty` reformats **all** of `include/`, `tests/` and `docs/examples/`;
review the diff and keep unrelated pre-existing files out of the commit.

## Archived experiments

Benchmarks, differential harnesses, measurement snapshots and the defect
reproductions for the retired codecs are on the
`legacy/reflection-experiments` branch, together with the build lines they used.
