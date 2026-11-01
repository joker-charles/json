# Draft comment for issue #5959 — opt-in comparison

> This is a comment-ready draft. It updates the annotation-only proposal in
> #5959 with the comparison requested in #5384. It has not been posted.

I built both opt-in styles on the same public-API-only core (no core-header
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

## Proposal

Keep the plain functions as the implementation surface, and make **per-type
`enable<T>` the primary opt-in**, with annotation as optional sugar:

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/reflection.hpp>

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

The one contract I would like your call on: **should annotation/`enable<T>`
refuse to compile when a hand-written ADL serializer is also visible, or is
"the opt-in owns the type" the documented behavior?** Silently ignoring a
custom `to_json` is the weakest point of the per-type routes today.

## Verification

- `GXX=g++-16 bash tests/static-reflection/optin_matrix.sh` — 10/10 groups
  pass (annotation baseline, namespace routes in split and `single_include`
  layouts, precedence, ODR TUs, both negative harnesses).
- `reflection_namespace.cpp` covers annotation, `enable<T>`, third-party,
  nested/container members, `ordered_json`, missing-key 403, non-object 304,
  extra-key ignore, and direct qualified calls.
- `optin_conflicts.cpp` covers annotation/`enable`/`using` × custom ADL /
  `NLOHMANN_DEFINE_TYPE_*` / explicit `adl_serializer`.
- The existing annotation doctest target still passes: 4 cases / 35 assertions.

## Questions

1. Namespace name: `nlohmann::reflection`?
2. `enable<T>` spelling/mechanism: variable template, trait class, or an ADL
   customization point that does not open `nlohmann`?
3. Annotation: keep as sugar, or drop?
4. Custom-serializer conflict: compile-time diagnostic or documented
   "opt-in wins"?
5. Namespace-wide `using`: keep as an experimental mode, or drop it?
6. One header or two (`reflection.hpp` + `reflection_namespace.hpp`)?
