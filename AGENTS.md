# AGENTS.md — nlohmann/json · feature/static-reflection branch

This file captures the working conventions for the `feature/static-reflection`
branch. It is the authoritative operational guide for any agent/human entering
this branch: what the work is, which toolchain to trust, how to build/reproduce
the verified fixtures, and — crucially — the safety boundaries you must not
cross when touching the main library.

> **Scope.** The repository is nlohmann/json (3.12.0). This document describes
> the **branch-specific experimental work** on top of `develop` (cdf52ae9). It
> does not restate upstream contribution rules (see `.github/CONTRIBUTING.md`).

## 1. What this branch is

Two coupled goals:

1. **Static reflection (P2996)** — explore rewriting the tagged-union core with
   C++26 reflection, as a self-contained study (not a replacement of the C++11
   library).
2. **C++20 concepts modernization** — replace hand-written SFINAE detection with
   concepts for the pure-category overloads in the serialization layer, under a
   `JSON_HAS_CPP_20` dual path.

Status: M0–M3 (reflection) complete & verified; M4 verdict: reflection does not
replace type_traits, classification in `docs/static-reflection/M4_ASSESSMENT.md`;
to_json concepts dual-path landed and committed. See the doc map (§6).

## 2. Toolchain — trust, don't re-derive

- Use **`g++-16`** (GCC 16.1.0). The system default `g++` is 15 and **has no
  `<meta>`** (nor other C++26 headers) — reflection code will fail with
  `fatal error: meta: No such file or directory` if compiled with the wrong
  compiler.
- Reflection build flags: `-std=c++26 -freflection`.
- Smoke test (one line, cheap confidence — not a full re-verification):
  ```sh
  printf 'int main(){}' | g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t && echo "g++-16 OK"
  ```
- CMake presets (`CMakeUserPresets.json`, gitignored): `cxx26-reflect` /
  `cxx26-reflect-tests` pin `g++-16` + `CMAKE_CXX_STANDARD=26` (+
  `JSON_TestStandards=26` for the suite). **Do NOT put `-freflection` into
  `CMAKE_CXX_FLAGS`** — it leaks into the nested-project cmake tests
  (`tests/cmake_add_subdirectory` etc.), whose `TryCompile` probes compile with
  the *default* standard and die with `'-freflection' only supported with
  '-std=c++26'` (this exact failure broke 13 ctest cases until the preset was
  fixed). Reflection is consumer-side: add `-freflection` only to the TUs that
  actually use P2996 (the reflection fixtures do this via their own build
  lines).
- Two include layouts: split headers under `-Iinclude`, or the amalgamated
  single header under `-Isingle_include`. Reflection/concepts work targets the
  split `-Iinclude` layout (the `#ifdef JSON_HAS_CPP_20` branches only compile
  there).

## 3. Facts verified on this toolchain (do not re-verify)

Same toolchain ⇒ trust these; re-deriving them is wasted work.

- **Reflection/P2996** (verified by `tests/static-reflection/probe_*.cpp`):
  - `std::meta::` must be fully qualified; bare `meta::` does not compile.
  - `basic_json::json_value` / `basic_json::data` are **private nested types**.
    A direct scope-splice `[: ^^ json::json_value :]` FAILS ("is private within
    this context"). **BUT** — verified by `probe_real_json_value.cpp` — they are
    fully reachable indirectly: `unchecked()` reflection enumerates the private
    data members (`data` via `m_data`, then `json_value` via `data::m_value`),
    `type_of` yields the private nested type, and it can be spliced for full use
    (enumerate members, construct via `value_t`, read/write members, allocate/
    free pointer members). **Key GCC-16 trap**: obtaining the type works both
    from a `consteval` function and inside `template for`, but *enumerating its
    members* works ONLY from a `consteval` context — inside `template for` the
    indirectly-obtained type reports "not a complete class type". So the
    `json_value_mirror` route is a workaround, not a hard requirement; a
    consteval-helper design can reflect the real `json_value` directly.
  - `nonstatic_data_members_of` works on a union; `is_pointer_v<type_of(m)>`
    classifies storage category.
  - Reflection queries returning a `std::vector` are **transient**: subscript/
    size directly on the call; never bind to a local constexpr (GCC rejects with
    "refers to a result of 'operator new'").
  - `template for` exists only in range form; the range must be an inline
    `define_static_array(...)` expression or a namespace-scope/static constexpr.
  - `if constexpr` does NOT discard the false branch; dispatch by tag overload /
    partial specialization.
- **Concepts dual-path** (verified by `concepts_smoke.cpp` + layered probes):
  - `concepts::array_like/object_like/string_like` are **byte-identical** to the
    traits only when they use the *exact* probe targets: for `array_like` the
    iterator must come from **`begin-range` (`is_range`/`range_value_t`)**, NOT
    `T::iterator` — range views like `std::ranges::reverse_view<ref_view<json>>`
    have `begin()` but no `iterator` alias; the baseline trait accepts them while
    a `T::iterator` probe wrongly rejects them (a real drift that broke
    `test-iterators2_cpp20`, fixed in commit 4fe8a702). Use `is_constructible`
    (not `convertible_to`), and preserve the `vector<uint8_t>`-is-not-binary
    special case. A "semantic restatement" drifts (see `probe_draft_drift.cpp`:
    1 real drift on `vector<uint8_t>` binary).
  - `#if` cannot appear inside a `requires` clause ⇒ gate the range-view
    exclusion through a `bool` variable template (`not_range_view`).
  - A multi-condition `requires` chain must be wrapped in parentheses:
    `requires (A && B && ...)`.
  - **Partial-application concepts carry `BasicJsonType` LAST.** `string_like` /
    `object_like` / `array_like` are declared `template<T /*candidate*/, B
    /*BasicJsonType*/>`. GCC resolves a constrained placeholder
    `concepts::string_like<BasicJsonType> S` by binding `S` to the FIRST param
    and the explicit `<BasicJsonType>` to the SECOND — declaring them `<B, T>`
    puts the wrong type in `B` and `typename B::string_t` fails on a user type
    like a custom `alt_string` string_t (real regression: `test-alt-string_cpp26`
    failed; verified minimal repro + `concepts::X<T, B>` order fixed both the
    placeholder form and the explicit `requires(X<T, B> ...)` form). Never
    reorder these back to `<B, T>`. Do NOT leave this class of bug to be caught
    only by a non-default-string_t TU: `concepts_smoke`/dual tests use
    `std::string` where the wrong binding happens to agree, so they miss it.

## 4. Build & reproduce

Reference for the artifacts (all under `g++-16`):

```sh
# M1 enum reflection + probes
g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude -o /tmp/p repro_m1.cpp
# M2/M3 differential (vs the real library) + ASan leak check
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
# M4 concepts dual-path smoke (compile for -std=c++11/20/26; outputs must match)
g++-16 -std=c++20 -O0 -Iinclude -o /tmp/s tests/static-reflection/concepts_smoke.cpp && /tmp/s
```

The full fixture list lives in `tests/static-reflection/`; each file documents
its own build line in its header comment. The main-library changes are also
covered by the repository's doctest unit suite — see §5 "Repository test suite".

## 5. Safety boundaries (non-negotiable)

- **Main library `include/nlohmann/detail/conversions/*.hpp` and the traits layer
  are C++11-contract.** Any change must compile cleanly across
  `-std=c++11/14/17/20/26`, or be gated behind `#ifdef JSON_HAS_CPP_20` with a
  full C++11 fallback. The dual-track pattern in those files is the template to
  follow.
- **`single_include/nlohmann/json.hpp` is an amalgamated build artifact**, a
  snapshot of the split headers. Do not edit it directly for feature work; if you
  touch a split header read into the single header, **re-amalgamate** so the
  single header stays in sync:
  ```sh
  make amalgamate   # amalganmate.py + `make pretty` (astyle 3.4.13 via tools/astyle/venv)
  ```
  astyle 3.4.13 (the CI-pinned version) and the venv are set up in this
  workspace (`tools/astyle/venv`, gitignored), so `make amalgamate` and `make
  pretty` are fully reproducible locally. If you only need the single header
  regenerated without the astyle pass, run:
  ```sh
  python3 tools/amalgamate/amalgamate.py -c tools/amalgamate/config_json.json -s .
  ```
  Note: `make pretty` formats **all** `include/`, `tests/`, and `docs/examples/`
  sources; review the diff and keep unrelated pre-existing files out of the
  commit (see da68ff71 for the cleanup of docs/examples/parser_callback_t.cpp).
- **Repository test suite.** The main-library changes are exercised by the
  repository's doctest suite. Configure and build a subset, e.g.:
  ```sh
  cmake -S . -B build-test -DCMAKE_CXX_COMPILER=g++-16 \
    -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
  cmake --build build-test --target test-udt_cpp11 test-serialization_cpp11 \
    test-conversions_cpp17 test-concepts_dual_cpp20 -j2
  ./build-test/tests/test-concepts_dual_cpp20
  ```
  `tests/src/unit-concepts_dual.cpp` (mentions `JSON_HAS_CPP_20`) registers
  both `_cpp11` and `_cpp20` targets and proves the dual path is behavior-preserving.
- **Free-experiment region** (safe to change freely): `concepts.hpp`,
  `reflection_json.hpp`, everything under `tests/static-reflection/`, and the
  docs under `docs/static-reflection/`.
- **Every new overload / concept / trait rewrite must be validated** by the
  differential / zero-drift probes outlined in `M4_ASSESSMENT.md` §7. The burden
  is on the change to prove byte-identical behavior, not on reviewers to trust it.

## 6. Doc map

- `docs/static-reflection/FEASIBILITY.md` — P2996 reflection redesign (M0–M3),
  the mirror-union route, verified patterns & pitfalls.
- `docs/static-reflection/M4_ASSESSMENT.md` — type_traits classification
  (A/B/C classes), the layered concepts strategy, composite-concept verdicts.
- Authoritative C++ feature/API facts (header map, signatures, reflection index):
  the **`modern-cpp` skill**; the local offline cppreference if present.

## 7. Commit conventions

- One logical change per commit; semantic message describing the "why".
- Keep the main-library change, the experimental-library change, and the docs in
  separate commits so history stays reviewable (see the existing 3-commit series
  on this branch).
- **Every commit on this branch carries a DCO sign-off** (`Signed-off-by:`
  line) so the history remains DCO-clean per `.github/CONTRIBUTING.md`.
  Verify with `git log --format='%h %s%n%b' -1 <sha> | grep Signed-off-by`.
- The branch is a work-in-progress experiment; commits are local until you push.
