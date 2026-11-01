# AGENTS.md — nlohmann/json · feature/static-reflection branch

Short entry point for anyone (agent or human) entering this branch. Rules live
here; reference material is loaded on demand from `docs/static-reflection/`.
Upstream contribution rules are in `.github/CONTRIBUTING.md` and are not
repeated.

## 1. What this branch is

One goal: a **small, opt-in, C++26 static-reflection add-on** that serializes
annotated flat structs through the public `basic_json` API, proposed upstream as
a reviewable first PR.

Current state: the upstream-facing reflection code is
`include/nlohmann/reflection.hpp` (explicit add-on gated by
`JSON_USE_REFLECTION`, per-type annotation opt-in). The four core headers and
`single_include/nlohmann/json.hpp` are byte-identical to `develop`;
`tests/CMakeLists.txt` is `develop` plus one C++26 doctest target.

On the **`reflection-opt-in-compare` experiment branch only**, there is a second
add-on: `include/nlohmann/reflection_namespace.hpp` adds the reflection-namespace
opt-in routes (`nlohmann::reflection::enable<T>`, namespace `using`, optional
annotation sugar) on the same public-API-only core. It is evidence for the
maintainer's comparison request in discussion #5384, not part of upstream PR 1.
See `docs/static-reflection/OPT_IN_COMPARISON.md`; do not fold it into the
annotation PR until the opt-in style is decided.

Retired: a wide-open catch-all, a `refl2` annotation codec, enum mapping,
reflection-driven binary formats and a reflection-backed tagged union — all of
it depended on `basic_json`'s private layout through `unchecked()`, which
upstream has ruled out. That work is on the `legacy/reflection-experiments`
branch; do not reintroduce it here.

## 2. Safety boundaries (non-negotiable)

- **`include/nlohmann/detail/**` and the traits layer are C++11-contract.** Any
  change compiles under `-std=c++11/14/17/20/26`, or is gated with a full
  fallback. This branch should not change them at all.
- **`single_include/nlohmann/json.hpp` is a build artifact.** Never edit it by
  hand; if a split header read into it changes, re-amalgamate
  (`make amalgamate`). The reflection add-on is deliberately not amalgamated.
- **The doctest suite stays green**, and the extension's own gates must hold:
  the runtime fixture (both include layouts), `compile_fail.sh`,
  `namespace_compile_fail.sh`, `optin_matrix.sh`, `check_docs_examples.sh` and
  `test-static_reflection_cpp26`.
- **No behavior claims without proof.** Anything asserted as byte-identical must
  show the comparison; the compile-fail harness exists so rejection paths are
  proven, not assumed.
- **Free-experiment region**: `include/nlohmann/reflection.hpp`,
  `include/nlohmann/reflection_namespace.hpp`, `tests/static-reflection/`,
  `docs/static-reflection/`.
- **Scratch files** go in `.tmp/` or `build/scratch/` (both gitignored). `/tmp`
  does not survive between tool invocations here.

## 3. Commits

- One logical change per commit; semantic message explaining *why*.
- Every commit carries a DCO sign-off (`Signed-off-by:`).
- **Pushing** needs escalated sandbox permissions (the default policy blocks the
  network and ssh). Both configured remotes are credential-less HTTPS mirrors,
  so push to the fork over SSH with an explicit URL — never to `origin`, which
  is the upstream project:
  `git push git@github.com:joker-charles/json.git <branch>`

## 4. Read when

Load the **`modern-cpp` skill** before writing or reviewing any C++ here —
reflection, concepts, serialization, or a toolchain/build judgement.

| When | Read |
|---|---|
| Writing reflection code, or a `std::meta` error looks like a compiler bug | `docs/static-reflection/VERIFIED_FACTS.md` |
| The extension's contract, semantics or rejected cases | `docs/static-reflection/EXTENSION.md` |
| Build/test commands, upstream plan, history | `docs/static-reflection/README.md` |
| The public-facing description | `docs/mkdocs/docs/features/reflection.md` |
