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
- Two include layouts: split headers under `-Iinclude`, or the amalgamated
  single header under `-Isingle_include`. Reflection/concepts work targets the
  split `-Iinclude` layout (the `#ifdef JSON_HAS_CPP_20` branches only compile
  there).

## 3. Facts verified on this toolchain (do not re-verify)

Same toolchain ⇒ trust these; re-deriving them is wasted work.

- **Reflection/P2996** (verified by `tests/static-reflection/probe_*.cpp`):
  - `std::meta::` must be fully qualified; bare `meta::` does not compile.
  - `basic_json::json_value` / `basic_json::data` are **private**, and even
    `access_context::unchecked()` cannot make a scope-splice
    `[: ^^ json::json_value :]` reference them ⇒ reflection targets a **mirror
    union** (`json_value_mirror`).
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
    traits only when they use the *exact* probe targets: `T::iterator` (not
    `value_type`), `is_constructible` (not `convertible_to`), and preserve the
    `vector<uint8_t>`-is-not-binary special case. A "semantic restatement" drifts
    (see `probe_draft_drift.cpp`: 1 real drift on `vector<uint8_t>` binary).
  - `#if` cannot appear inside a `requires` clause ⇒ gate the range-view
    exclusion through a `bool` variable template (`not_range_view`).
  - A multi-condition `requires` chain must be wrapped in parentheses:
    `requires (A && B && ...)`.

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
its own build line in its header comment.

## 5. Safety boundaries (non-negotiable)

- **Main library `include/nlohmann/detail/conversions/*.hpp` and the traits layer
  are C++11-contract.** Any change must compile cleanly across
  `-std=c++11/14/17/20/26`, or be gated behind `#ifdef JSON_HAS_CPP_20` with a
  full C++11 fallback. The dual-track pattern in those files is the template to
  follow.
- **`single_include/nlohmann/json.hpp` is an amalgamated build artifact**, a
  snapshot of the split headers. Do not edit it directly for feature work; only
  regenerate it via the amalgamate tool if you intentionally ship to that layout.
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
- The branch is a work-in-progress experiment; commits are local until you push.
