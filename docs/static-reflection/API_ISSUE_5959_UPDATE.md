# Draft comment for issue #5959 — opt-in comparison

> This is a comment-ready draft. It updates the annotation-only proposal in
> #5959 with the comparison requested in #5384. It has not been posted.

I built the opt-in routes on the same public-API-only core (no core-header
changes, no private `json_value` reflection) and compared them. Code, fixtures
and the full result table are on the `reflection-opt-in-compare` experiment
branch; a summary follows.

## What was compared

| Route | How a type opts in | Needs `using`? | Works for a type you don't own? |
|---|---|---|---|
| A annotation | `struct [[=nlohmann::json_serializable{}]] S` | no | no |
| B1 `enable<T>` | `template<> inline constexpr bool enable<S> = true;` in `nlohmann::reflection` | no | **yes** |
| B2 namespace `using` | `using nlohmann::reflection::to_json/from_json;` in the type's namespace | yes | yes, if the namespace can be reopened |

B1 and B2 both call the same plain functions
`nlohmann::reflection::to_json/from_json`; annotation is now optional sugar over
the same core. B2 has a constrained and an unconstrained mode: only the
unconstrained mode makes `using` an opt-in by itself.

## Results

1. **Third-party types are the decisive difference.** `enable<T>` is one
   declaration in a serialization header and requires neither an annotation nor
   reopening the type's namespace. That is the case annotation cannot serve.
2. **`using` alone is namespace-wide, not per-type.** With the functions
   constrained on `enable<T>`/annotation, `using` alone does not opt a type in;
   it is only a visibility declaration. With unconstrained functions, `using`
   opts in every class in the namespace, and unsupported types fail inside the
   serializer body — the caveat you raised is real and is covered by
   `namespace_compile_fail/cf_wide_private_member.cpp`.
3. **Precedence differs by route.** Annotation and `enable<T>` select an
   `adl_serializer` specialization and therefore bypass a hand-written ADL
   `to_json`/`from_json`. Namespace `using` goes through ADL, so the
   non-template custom serializer wins. A fully generic custom `to_json` plus
   the unconstrained `using` route is ambiguous.
4. **ODR/visibility.** `using` and `enable<T>` must be visible before first use
   and consistently in every TU; a two-TU fixture shows `is_constructible`
   disagreeing when one TU does not see the enable. Annotation travels with the
   type definition and does not have this hazard.
5. **Same-core codegen.** A 40-type `-O2` object is byte-size identical for A and
   B1 (text 76,991 / data 136 / bss 0), as expected from the shared core. This is
   **not** a claim of zero overhead versus the macro path: the legacy branch
   already measured that reflection carries a per-type cost; see the branch's
   evidence appendix.
6. **Diagnostics.** Enabled types get the same targeted `static_assert` as
   annotation for union/base/private/reference/const/unsupported members.
   Reference and const members are rejected explicitly rather than failing in
   the library's `get<T&>()` internals.
7. **Wire contract.** Missing key → 403, non-object/array/null input → 304,
   extra keys ignored; a missing key for `std::optional` is 403, not `nullopt`;
   a present `null` still gives `nullopt`.
8. **Complement, not replacement.** `WITH_NAMES`, `WITH_DEFAULT` and
   `ONLY_SERIALIZE` have no reflection-v1 equivalent (`migration_matrix.cpp`),
   so a drop-in replacement would need later slices.
9. **Delegation is the remaining hard constraint.** A type's own
   `to_json`/`from_json` that delegates to `nlohmann::reflection::to_json` is
   **bypassed** by annotation, `enable<T>`, and `using` + `enable<T>` (the
   `adl_serializer` specialization wins). It runs only with unconstrained
   namespace `using` (`delegation_matrix.cpp` / `delegation_matrix_wide.cpp`).
   So the per-type `enable<T>` route cannot satisfy the "own serializer
   delegates" use case without a design change.

## Proposal

Keep the plain functions as the implementation surface. The current
recommendation is **per-type `enable<T>` for plain types**, with annotation as
optional sugar; namespace `using` stays an explicit experimental mode:

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/reflection.hpp>
#include <nlohmann/reflection_namespace.hpp>   // experiment branch; may be merged

// owned type: one-line sugar (or the enable<T> form)
struct [[=nlohmann::json_serializable{}]] point { double x, y; };

// unowned type: one-line registration, no annotation, no `using`
namespace nlohmann::reflection
{
template<> inline constexpr bool enable<third_party::widget> = true;
}
```

I would **not** make namespace-wide `using` the default opt-in: in the
constrained form it is only visibility, and in the unconstrained form it changes
behavior for every type in the namespace. It can stay available as an explicit
experimental mode.

**The one API-shape decision I cannot make for you is delegation.** The
maintainer's use case "call the plain functions from a type's own
`to_json`/`from_json`" is not satisfied by annotation or `enable<T>`: their
`adl_serializer` specialization runs instead of the wrapper. The options are:

1. use unconstrained namespace `using` for such types, accepting the
   whole-namespace caveat;
2. remove the forwarding specialization and require `using` + `enable<T>`
   (two declarations; the custom serializer wins and can delegate);
3. declare the delegation pattern out of scope and document that the opt-in
   owns the type.

If delegation is in scope, option 2 may be the cleanest compromise; if it is
not, the current C2 recommendation stands.

## Note on the issue body

The original #5959 body is annotation-only and predates the comparison. Two
prototype claims there are now stale: the feature-macro gate and the GCC 16 CI
job are implemented on the feature branch, and the negative-compile harness now
has 13 annotation + 15 namespace cases (including reference/const guards). This
comment is the comparison requested in #5384; the body can be updated after the
opt-in surface is settled.

## Verification

- `GXX=g++-16 bash tests/static-reflection/optin_matrix.sh` — 15/15 groups
  pass (annotation baseline, namespace routes in split and `single_include`
  layouts, precedence, boundaries, semantics, migration, delegation, ODR TUs,
  both negative harnesses).
- `reflection_namespace.cpp` covers annotation, `enable<T>`, third-party,
  nested/container members, `ordered_json`, missing-key 403, non-object 304,
  extra-key ignore, and direct qualified calls.
- `optin_conflicts.cpp` covers annotation/`enable`/`using` × custom ADL /
  `NLOHMANN_DEFINE_TYPE_*` / explicit `adl_serializer`.
- `boundary_matrix.cpp`, `semantics_matrix.cpp`, `migration_matrix.cpp` and the
  two delegation fixtures cover the contract and the macro gaps.
- The existing annotation doctest target still passes: 4 cases / 35 assertions.

## Questions

1. Namespace name: `nlohmann::reflection`?
2. `enable<T>` spelling/mechanism: variable template, trait class, or an ADL
   customization point that does not open `nlohmann`?
3. Annotation: keep as sugar, or drop?
4. **Delegation:** is "a type's own `to_json`/`from_json` delegates to the
   plain functions" in scope? If yes, choose unconstrained `using` (caveat),
   or the no-forwarding `enable<T>` + `using` variant (two declarations), or
   leave it out.
5. Custom-serializer conflict (if delegation is out of scope): compile-time
   diagnostic or documented "opt-in wins"?
6. Namespace-wide `using`: keep as an experimental mode, or drop it?
7. One header or two (`reflection.hpp` + `reflection_namespace.hpp`)?
