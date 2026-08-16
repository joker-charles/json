# AGENTS.md — nlohmann/json · feature/static-reflection branch

This file is the short entry point for any agent/human entering the
`feature/static-reflection` branch. It states what the branch is, the
non-negotiable safety boundaries, the commit conventions, and — in §4 —
**which reference document to read when** (progressive disclosure: the long
reference material lives in `docs/static-reflection/` and is loaded on
demand, not up front).

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
to_json concepts dual-path landed and committed. **M5 shipped**: the refl2
codec moved into `include/nlohmann/reflection_to_json.hpp` (new experimental
header) with a `json_name`/`json_ignore`/`json_default` annotation layer, and
is wired into `detail::{to,from}_json` as reflection-gated catch-alls
(`__cpp_impl_reflection && __cpp_lib_reflection`, i.e. only g++-16
`-std=c++26 -freflection`) — arbitrary reflectable structs serialize
bidirectionally with zero user code, replacing the NLOHMANN_DEFINE_TYPE_*
macro family (Scenario B of EVALUATION.md); **M6**: enumerator-level
`json_name` annotations replace NLOHMANN_JSON_SERIALIZE_ENUM (compile-time
enumerator tables; unannotated enums keep the integer path, zero drift).
Design: `docs/static-reflection/M5_REFLECTION_TO_JSON.md`; verified facts:
`docs/static-reflection/VERIFIED_FACTS.md`.

## 2. Safety boundaries (non-negotiable)

Rules first — the exact commands for the highlighted items are in
`docs/static-reflection/BUILD_RECIPES.md`.

- **Main library `include/nlohmann/detail/conversions/*.hpp` and the traits layer
  are C++11-contract.** Any change must compile cleanly across
  `-std=c++11/14/17/20/26`, or be gated behind `#ifdef JSON_HAS_CPP_20` with a
  full C++11 fallback (newer reflection gates follow the same pattern: full
  fallback when the gate is off). The dual-track pattern in those files is the
  template to follow.
- **`single_include/nlohmann/json.hpp` is an amalgamated build artifact.** Do
  not edit it directly for feature work; if you touch a split header read into
  the single header, **re-amalgamate** so the single header stays in sync
  (recipe: `BUILD_RECIPES.md` §"Single-header regeneration"). `make pretty`
  reformats ALL sources — review the diff and keep unrelated pre-existing files
  out of the commit.
- **Repository test suite.** Main-library changes must keep the doctest suite
  green (recipe: `BUILD_RECIPES.md` §"Repository test suite").
- **Free-experiment region** (safe to change freely): `concepts.hpp`,
  `reflection_json.hpp`, `reflection_to_json.hpp`, everything under
  `tests/static-reflection/`, and the docs under `docs/static-reflection/`.
- **Every new overload / concept / trait rewrite must be validated** by the
  differential / zero-drift probes outlined in `M4_ASSESSMENT.md` §7. The burden
  is on the change to prove byte-identical behavior, not on reviewers to trust it.
- **Session scratch files live in `.tmp/` or `build/scratch/` (both
  gitignored).** `/tmp` does not survive across tool invocations in this
  environment — never rely on it for anything you need again (experiment
  TUs, negative compile cases, one-off probes, measurement artifacts).
  `.tmp/` is for session drafts/experiments, `build/scratch/` for
  measurement artifacts (bench binaries, driver output). Anything
  reproducible belongs in the repo: fixtures under
  `tests/static-reflection/`, measurement snapshots under
  `docs/static-reflection/data/` (see EVALUATION.md §5).

## 3. Commit conventions

- One logical change per commit; semantic message describing the "why".
- Keep the main-library change, the experimental-library change, and the docs in
  separate commits so history stays reviewable (see the existing 3-commit series
  on this branch).
- **Every commit on this branch carries a DCO sign-off** (`Signed-off-by:`
  line) so the history remains DCO-clean per `.github/CONTRIBUTING.md`.
  Verify with `git log --format='%h %s%n%b' -1 <sha> | grep Signed-off-by`.
- The branch is a work-in-progress experiment; commits are local until you push.

## 4. Progressive reading guide (read the reference on demand)

| When | Read |
|---|---|
| Before writing ANY reflection/concepts/serialization code | `docs/static-reflection/VERIFIED_FACTS.md` — verified compiler/type facts (do not re-verify); generic C++26 facts live in the `modern-cpp` skill |
| Configuring, building, reproducing, measuring | `docs/static-reflection/BUILD_RECIPES.md` — toolchain, fixture build lines, amalgamate, test suite |
| Milestone design & verification methodology | `FEASIBILITY.md` (M0–M3) / `M4_ASSESSMENT.md` / `M5_REFLECTION_TO_JSON.md` / `EVALUATION.md` — see §5 |
| Working on the main library | §2 safety boundaries above + `BUILD_RECIPES.md` recipes |

## 5. Doc map

- `docs/static-reflection/VERIFIED_FACTS.md` — verified toolchain/compiler
  facts for this branch (the long §3 reference, loaded on demand).
- `docs/static-reflection/BUILD_RECIPES.md` — toolchain, fixture build lines,
  amalgamate, doctest suite recipes.
- `docs/static-reflection/FEASIBILITY.md` — P2996 reflection redesign (M0–M3),
  the mirror-union route, verified patterns & pitfalls.
- `docs/static-reflection/M4_ASSESSMENT.md` — type_traits classification
  (A/B/C classes), the layered concepts strategy, composite-concept verdicts.
- `docs/static-reflection/M5_REFLECTION_TO_JSON.md` — the reflection-driven
  bidirectional serializer replacing the NLOHMANN_DEFINE_TYPE_* macro family
  (M5) and the enum string mapping replacing NLOHMANN_JSON_SERIALIZE_ENUM
  (M6): the catch-all wiring, exact-probe exclusion sets, circularity
  defenses, the annotation layer, macro↔annotation semantic differences,
  and the verification matrix.
- Authoritative C++ feature/API facts (header map, signatures, reflection index):
  the **`modern-cpp` skill**; the local offline cppreference if present.
