# Upstream integration plan

> **Status (2026-10-29).** The branch carries exactly one candidate for
> upstream: `<nlohmann/reflection.hpp>`, a flat-struct serializer that is
> opt-in per type and uses only the public `basic_json` interface. Everything
> else that was explored here is on `legacy/reflection-experiments`.

## Agreed constraints

The nlohmann/json maintainer was asked whether an opt-in, C++26-only reflection
extension would be considered. The answer was yes, subject to these conditions.
They are the acceptance criteria for this branch; treat them as binding.

1. **A separate header, never included by default.** Users write
   `#include <nlohmann/reflection.hpp>` explicitly. It must not be pulled into
   `json.hpp` or into `single_include`. Without that include, nothing changes
   for anyone.
2. **Per-type opt-in.** The annotation form
   `struct [[=nlohmann::json_serializable{}]] S` is the approved direction.
   Types that are not annotated must behave exactly as they do today.
3. **Public API only.** Reflecting into `basic_json`'s private members
   (`json_value` via `unchecked()`) is a no-go: it is not a contract, and the
   extension must not silently depend on internal layout. Values are built and
   read through the normal public interface, exactly as a hand-written
   `to_json`/`from_json` would.
4. **No core-header changes in the same effort.** `lexer.hpp`,
   `binary_reader.hpp`, `to_json.hpp`/`from_json.hpp` and any C++20 concepts
   layer stay out of scope. The extension only *adds* headers. The
   binary-format work (msgpack/UBJSON/BSON via reflection) also stays out for
   now.
5. **Start small.** The first PR should be close to the 25-line flat-struct
   serializer: nested types through the normal ADL `to_json`/`from_json`, plus
   the actionable `static_assert` for unsupported members. Renaming/skipping
   members through annotations can follow once the core is settled. "A
   7,000-line codec isn't reviewable as one piece."
6. **CI and compilers.** Only GCC 16 implements P2996 today (`-freflection`),
   so a dedicated GCC 16 CI job for the extension is fine, but the feature
   stays **experimental until a second implementation exists** (most likely
   Clang). The API may change until then.
7. **Layout.** Tests go under `tests/src/unit-*.cpp` (doctest), gated on the
   feature macro. Documentation goes under `docs/mkdocs/docs/`.
8. **Process.** Open an issue proposing the minimal API — header name,
   annotation name, macro name — **before writing more code**, so the surface
   is agreed first.

## What PR 1 should contain

| File | State |
|---|---|
| `include/nlohmann/reflection.hpp` | new, ~200 lines; flat structs, public API only |
| `tests/src/unit-static_reflection.cpp` | new doctest TU, C++26 target |
| `tests/static-reflection/` (fixtures, compile-fail harness, docs check) | supporting harnesses — decide with the maintainer whether they ship in the PR or stay branch-local |
| `docs/mkdocs/docs/features/reflection.md` | new feature page |
| `docs/mkdocs/mkdocs.yml` | one nav entry |
| `tests/CMakeLists.txt` | one `-freflection` doctest target |
| `.github/workflows/*` | one GCC 16 job |

Explicitly **not** in PR 1: core-header edits, the concepts layer, annotation
semantics beyond opt-in, enum mapping, `std::variant`, binary formats, any
`unchecked()` use.

## Open questions for the issue

The issue must settle the naming surface before code review starts:

1. **Header name** — `<nlohmann/reflection.hpp>` is the current choice;
   alternatives: `<nlohmann/json_serializable.hpp>`.
2. **Annotation name** — `nlohmann::json_serializable`, spelled
   `struct [[=nlohmann::json_serializable{}]] S` (annotation follows the
   `struct` keyword; GCC 16 ignores the leading form with a warning).
3. **Macro name** — the current design needs **no** feature macro: including
   the header is the opt-in and the header is inert without P2996. The
   maintainer's condition mentions "behind a feature macro", so the issue must
   state this explicitly and ask whether header-include opt-in is acceptable,
   or whether a `JSON_USE_REFLECTION`-style gate is required on top.
4. **Namespace** — `nlohmann` (not a sub-namespace), consistent with the rest
   of the public API.

## Risks, honestly

- **One implementation only.** GCC 16's P2996 support is new; the second
  compiler may disagree on annotation placement or `std::meta` details. The
  header is written to fail loudly (static_assert) rather than silently omit.
- **The amalgamated header is not covered.** Users of `single_include` cannot
  use the extension; this is deliberate for PR 1 and should be stated in the
  docs.
- **The upstream testing bar is higher than this branch's.** Upstream wants
  `tests/src/unit-*.cpp`, coverage, and exception checks with
  `CHECK_THROWS_WITH_AS`. The current TU is a start, not a finished suite.

## Why this shape

The earlier attempt wired a wide-open catch-all into `detail/{to,from}_json`
and reached the private union layout. That is exactly the combination the
maintainer ruled out: it changes behavior for types that never opted in, and it
depends on an internal layout. The current header inverts both decisions — one
opt-in header, one annotation per type, public API only — at the cost of being
much smaller than what was explored here.
