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
enumerator tables; unannotated enums keep the integer path, zero drift);
**M4B**: the remaining binary formats of the value_t -> byte-code tables
milestone — reflection-driven MSGPACK / UBJSON (no-optimization mode) / BSON
writers in `reflection_json.hpp` (primary codes from the reflection-generated
`kMsgpackCodes`/`kUbjsonCodes`/`kBsonCodes` tables, differential-tested
byte-identical by `m4_binary.cpp`); **M4B-2**: the BSON READ direction —
`reflection_bson_parser` dispatches element types through the
reflection-generated `kBsonsLoad` reverse table (byte code -> {union slot,
payload kind}), the mirror image of the writer, differential-tested
byte-identical by `m4b_bson_reader.cpp`; **M4D**: the tagged-union API
surface is completed in `basic_json_reflection` — `type_name`/`size`/`empty`/
`at`/`erase`/`clear`/`swap`/all six comparison operators — plus the
reflection-generated value_t name & ordering tables (`kValueTNames`/
`kValueTTypeNames`/`kValueTWeights` + `value_t_order`/`value_t_less`) that
replace the hand-written `value_t.hpp order[]`/`type_name()` switch inside
the reflection header (identifier-keyed consteval, discarded stays
unordered), differential-tested by `m4d_api.cpp` (1411 checks, ASan clean);
**M4E**: the UBJSON write direction is completed — `reflection_ubjson_optimized_serializer`
adds the `'#'` count / `'$'` type optimized modes (use_type requires
use_count), the BJData dialect (all numbers/prefixes little-endian, the
`'u'`/`'m'`/`'M'` width rungs, the bjdx `'$'`-exclusion list, draft3's `'B'`
binary marker) and the JData ndarray encoding, differential-tested
byte-identical by `m4e_ubjson_opt.cpp` (630 checks, ASan clean); **M4B-3**:
the CBOR / MessagePack / UBJSON / BJData READ directions —
`reflection_cbor_parser` / `reflection_msgpack_parser` /
`reflection_ubjson_parser` dispatch through the `kCborLoads` /
`kMsgpackLoads` / `kUbjsonLoads` reverse tables, covering duplicate-key
last-wins, CBOR store-tag full uint64 subtypes, indefinite/nested strings,
UBJSON optimized containers, BJData ndarray decode, and library-lexer
high-precision numbers — differential-tested byte-identical by
`m4f_binary_readers.cpp` (671 checks, ASan clean); **M7**:
top-level `std::variant` support in the refl2 codec (`reflection_to_json.hpp`)
— the oneof wire format `{"index":N,"value":...}`, member recursion, and the
interaction with the M5 exclusion set/circularity defenses (a `json`
alternative stays excluded), verified by `probe_variant.cpp` (39 checks,
ASan clean); **M4D-2**: the tagged-union iteration surface —
`reflection_iterator<IsConst>` (object/array/primitive modes, mirroring
iter_impl's 12 switches via type routing), `begin`/`end`/`rbegin`/`rend`,
`operator[]` (null implicit conversion + array fill-up), `find`/`contains`/
`count`, `erase(iterator)`/`erase(first,last)` — differential-tested by
`m4d2_iterators.cpp` (50 checks, ASan clean).
**Hardening & alignment** (later session): refl2 cold-path fixes — the
array-like from_json gained an `insert` branch (std::set members were a
compile error) and `json_name` a consteval ctor with a key-length
static_assert; library-dedicated array types (`B::binary_t` / C array /
`std::forward_list` / `std::valarray` / `std::u8string`) now route through
the adl branch via `is_library_dedicated_array` (they were mis-serialized
as number arrays / static_assert / compile-error / abort); a negative
compile-fail harness (`compile_fail/`, 9 cases) plus runtime error-path
(`probe_negative_runtime.cpp`) and upstream type-surface
(`probe_type_alignment.cpp`) probes landed. The C++20 concepts modernization
is completed: the to_json boolean overload is `concepts::boolean_like<T,B>`
(the last pure-category overload left on enable_if). The main doctest suite
is green (test-udt_cpp11 / test-serialization_cpp11 / test-conversions_cpp17
/ test-concepts_dual_cpp20).
**Review pass (2026-08)**: a code review of the unmerged work found and fixed 20 defects —
most importantly that enabling `JSON_USE_REFLECTION` broke 4 upstream doctest TUs
(opaque enum -> `std::meta::exception`, lost `noexcept`, a self-dependent atomic
constraint, the M7 variant invariant), plus `std::abort()` on malformed input, a
dangling `rbegin()` reference, and JSON/CBOR/BSON output divergences. Top-level
`std::variant` is now gated behind `JSON_USE_REFLECTION_VARIANT`, gate-on regression
targets `test-reflection_*_cpp26` compile the upstream TUs with the gate enabled, and
the full report with verification commands is `docs/static-reflection/REVIEW_2026-08.md`.
Design:
`docs/static-reflection/M5_REFLECTION_TO_JSON.md` (M5/M6),
`docs/static-reflection/M4D_API_SURFACE.md` (M4D),
`docs/static-reflection/M4E_UBJSON_OPT.md` (M4E),
`docs/static-reflection/M4B3_BINARY_READERS.md` (M4B-3),
`docs/static-reflection/UPSTREAM_INTEGRATION_PLAN.md` (upstream integration strategy),
`docs/static-reflection/M7_VARIANT.md` (M7),
`docs/static-reflection/M4D2_ITERATORS.md` (M4D-2); verified facts:
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
- **Pushing from this sandboxed environment**: run `git push` with escalated
  sandbox permissions (`danger-full-access`). Under the default file policy it
  fails with
  `Bad owner or permissions on /etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf`
  because the sandbox presents the system ssh config as `nobody:nogroup` and
  ssh refuses to read it. The same command succeeds with escalation
  (verified: `git push origin feature/static-reflection` -> "Everything
  up-to-date"). Do not work around it by chowning the system file or by
  persisting `core.sshCommand` in the checkout.

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
