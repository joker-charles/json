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

One goal survives: a **small, opt-in, C++26 static-reflection add-on** that
serializes annotated flat structs through the public `basic_json` API, proposed
upstream as a reviewable first PR.

**Current state.** The only reflection code on the branch is
`include/nlohmann/reflection.hpp` — an explicit add-on that `json.hpp` and the
amalgamated single header do not include, opt-in per type
(`struct [[=nlohmann::json_serializable{}]] S`), public API only. The four core
headers that earlier work had modified (`detail/input/lexer.hpp`,
`detail/input/binary_reader.hpp`, `detail/conversions/{to,from}_json.hpp`) are
back to the `develop` baseline, the C++20 concepts layer is gone, and
`single_include/nlohmann/json.hpp` matches `develop`. `tests/CMakeLists.txt` is
`develop` plus one C++26 doctest target (`test-static_reflection_cpp26`).
Contract and test matrix: `docs/static-reflection/FLAT_STRUCT_REFLECTION.md`;
how it got here: `CORE_CLEANUP_2026-10.md`.

**Retired.** The branch previously carried a much larger exploration: a
wide-open `refl2` catch-all wired into `detail/{to,from}_json`, an annotation
layer (`json_name`/`json_ignore`/`json_default`), enum string mapping,
reflection-driven MessagePack/UBJSON/BSON/CBOR/BJData codecs, and a
reflection-backed tagged union. All of it reached `basic_json`'s private
`json_value` layout through `access_context::unchecked()` — which the upstream
maintainer has ruled out of scope — and all of it changed behavior for types
that never opted in. Its code, milestone reports, benchmarks, measurement
snapshots and defect reproductions are on the
**`legacy/reflection-experiments`** branch. Do not reintroduce them here.

Kept because it still applies: the verified compiler facts
(`VERIFIED_FACTS.md`), the build and toolchain recipes (`BUILD_RECIPES.md`),
the upstream plan with the maintainer's binding constraints
(`UPSTREAM_INTEGRATION_PLAN.md`), and the compile-fail / docs-check harnesses
under `tests/static-reflection/`.

**Next step**, per the maintainer's process: open an issue proposing the
minimal API (header name, annotation name, macro name) before writing more
code.

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
- **Free-experiment region** (safe to change freely): `reflection.hpp`, the
  remaining fixtures under `tests/static-reflection/`, and the docs under
  `docs/static-reflection/`. The legacy reflection code lives on the
  `legacy/reflection-experiments` branch — do not reintroduce it here.
- **Every behavior claim must be validated, not asserted.** The burden is on the
  change to prove it: for the extension that means the runtime fixture
  (`reflection_flat_struct.cpp`, including the `single_include` layout), the
  negative compile harness (`compile_fail.sh`), the docs check
  (`check_docs_examples.sh`), and the doctest target. Anything claiming
  byte-identical output must show the comparison.
- **Session scratch files live in `.tmp/` or `build/scratch/` (both
  gitignored).** `/tmp` does not survive across tool invocations in this
  environment — never rely on it for anything you need again (experiment
  TUs, negative compile cases, one-off probes, measurement artifacts).
  `.tmp/` is for session drafts/experiments, `build/scratch/` for
  measurement artifacts. Anything reproducible belongs in the repo: fixtures and
  harnesses under `tests/static-reflection/`.

## 3. Commit conventions

- One logical change per commit; semantic message describing the "why".
- Keep the main-library change, the experimental-library change, and the docs in
  separate commits so history stays reviewable (see the existing 3-commit series
  on this branch).
- **Every commit on this branch carries a DCO sign-off** (`Signed-off-by:`
  line) so the history remains DCO-clean per `.github/CONTRIBUTING.md`.
  Verify with `git log --format='%h %s%n%b' -1 <sha> | grep Signed-off-by`.
- The branch is a work-in-progress experiment; commits are local until you push.
- **Pushing from this sandboxed environment**: run `git push` with escalated
  sandbox permissions (`danger-full-access`); the default policy blocks the
  network and ssh refuses the sandbox-presented system ssh config. Both
  `origin` (nlohmann/json) and `upstream` (the fork) are configured as HTTPS
  mirrors (`ghfast.top`), which need credentials this workspace does not have.
  Push to the fork over SSH with an explicit URL instead — the key is already
  registered — without changing the remote configuration:
  `git push git@github.com:joker-charles/json.git <branch>`
  Never push to `origin`; it is the upstream project.

## 4. Progressive reading guide (read the reference on demand)

**Before writing or reviewing any C++ code in this repository, load the
`modern-cpp` skill** (`skill` tool, name `modern-cpp`). Do not write C++20+
code or make a toolchain/build judgement until it is loaded. It is a layered
bundle: read its router `SKILL.md` first, then follow its routing table to
**one** `references/` file for the task — do not read them all.

The trigger is the *kind of decision*, not the language standard: this
repository keeps C++11–C++26 green, so "it's only C++20 syntax" is not a
reason to skip it. Load it when you are about to:

- write/modify anything using P2996 reflection, P2900 contracts, or `#embed`;
- use a C++20+ library facility (`format`, `expected`, `span`, `ranges`,
  `print`, `mdspan`, `inplace_vector`, …);
- resolve a build/toolchain error (`meta: No such file or directory`,
  `'-freflection' only supported with '-std=c++26'`, "C++23 used silently", …);
- design a reflection-driven serializer/codec;
- pick a header or an exact API spelling instead of guessing it.

| When | Read |
|---|---|
| Before writing ANY reflection/concepts/serialization code | `docs/static-reflection/VERIFIED_FACTS.md` — verified compiler/type facts (do not re-derive them); generic C++26 facts live in the `modern-cpp` skill |
| Configuring, building, reproducing, measuring | `docs/static-reflection/BUILD_RECIPES.md` — toolchain, fixture build lines, amalgamate, test suite |
| The extension's contract / what to propose upstream | `FLAT_STRUCT_REFLECTION.md` / `UPSTREAM_INTEGRATION_PLAN.md` — see §5 |
| Working on the main library | §2 safety boundaries above + `BUILD_RECIPES.md` recipes |

## 5. Doc map

- `docs/static-reflection/FLAT_STRUCT_REFLECTION.md` — the supported
  extension: per-type `[[=nlohmann::json_serializable{}]]`, public-API-only
  serializer, scope, test matrix, non-goals.
- `docs/static-reflection/UPSTREAM_INTEGRATION_PLAN.md` — what PR 1 contains,
  the maintainer's binding constraints, the open naming questions for the issue.
- `docs/static-reflection/VERIFIED_FACTS.md` — verified toolchain and P2996
  facts for this repo (do not re-derive).
- `docs/static-reflection/BUILD_RECIPES.md` — toolchain, fixture build lines,
  harnesses, doctest target, amalgamate rules.
- `docs/static-reflection/CORE_CLEANUP_2026-10.md` — how the branch reached its
  current shape (core restored, legacy code parked).
- `docs/static-reflection/PRIVATE_MEMBER_REFLECTION_PLAN.md` — planned next
  slice for explicit private-member access while keeping `basic_json` on the
  public API.
- Authoritative C++ feature/API facts (header map, signatures, reflection index):
  the **`modern-cpp` skill**; the local offline cppreference if present.
