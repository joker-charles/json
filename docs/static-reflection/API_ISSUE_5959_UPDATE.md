## Opt-in comparison (requested in #5384)

Following your reply in #5384, I built the three opt-in routes on the same
public-API-only core (no core-header changes, no private `json_value`
reflection) and compared them. The code, fixtures and full result table are on the
[`reflection-opt-in-compare`](https://github.com/joker-charles/json/tree/reflection-opt-in-compare) experiment branch. The summary below is
the proposed direction.

### What was compared

| Route | How a type opts in | Needs `using`? | Works for a type you don't own? |
|---|---|---|---|
| C1 annotation | `struct [[=nlohmann::json_serializable{}]] S` | no | no |
| C2 `enable<T>` | `template<> inline constexpr bool enable<S> = true;` in `nlohmann::reflection` | no | **yes** |
| C3 namespace `using` | `using nlohmann::reflection::to_json/from_json;` in the type's namespace | yes | yes, if the namespace can be reopened |

All three call the same `nlohmann::reflection::to_json/from_json` core;
annotation is optional sugar. C3 has a constrained and an unconstrained mode:
only the unconstrained mode makes `using` an opt-in by itself. The proposal
below uses the unconstrained mode.

### At a glance

| Criterion | C1 annotation | C2 `enable<T>` | C3 `using` (unconstrained) |
|---|---|---|---|
| Opt-in unit | type | type | namespace |
| Third-party type | no | **yes** | yes, if the namespace can be reopened |
| Type definition depends on nlohmann | yes | no | no |
| Needs `using` | no | no | yes (using-declaration) |
| Custom serializer / own-serializer delegation | bypassed | bypassed | **composes / runs** |
| ODR/visibility | travels with the type | external, shared header | external + using declaration |
| Unsupported type after opt-in | targeted `static_assert` | targeted `static_assert` | hard error in the serializer body |
| Recommendation | optional sugar | optional safety mode | **primary** |

The recommendation follows from the delegation row: C3 is the only route where
a type's own serializer can call the plain functions. The cost is the
namespace-wide caveat; C2 remains the safe per-type fallback.

### Results

1. **Third-party types.** `enable<T>` is one declaration in a serialization
   header and requires neither an annotation nor reopening the type's
   namespace. C3 also covers third-party types, but only when their namespace
   can be reopened to add the using-declaration.

2. **`using` is namespace-wide, not per-type.** With unconstrained functions,
   `using` opts in every complete class/union in that namespace; unsupported
   types fail inside the serializer body. This is the caveat you raised, and it
   is covered by `namespace_compile_fail/cf_wide_private_member.cpp`. A
   non-template custom serializer still wins, so a type with its own
   `to_json`/`from_json` is unaffected.

3. **Precedence differs by route.** Annotation and `enable<T>` select an
   `adl_serializer` specialization and therefore bypass a hand-written ADL
   `to_json`/`from_json`. Namespace `using` goes through ADL, so the
   non-template custom serializer wins. A fully generic custom `to_json` plus
   the unconstrained `using` route is ambiguous.

4. **Delegation is the decisive difference for the primary route.** A type's
   own `to_json`/`from_json` that delegates to `nlohmann::reflection::to_json`
   is **bypassed** by annotation, `enable<T>`, and `using` + `enable<T>`; it
   runs only with unconstrained namespace `using`
   (`delegation_matrix.cpp` / `delegation_matrix_wide.cpp`). This is why the
   proposed primary is C3, not C2.

5. **ODR/visibility.** `using` and `enable<T>` must be visible before first use
   and consistently in every TU; a two-TU fixture shows `is_constructible`
   disagreeing when one TU does not see the opt-in. Annotation travels with the
   type definition and does not have this hazard.

6. **Same-core codegen.** A 40-type `-O2` object is byte-size identical for C1
   and C2 (text ≈77k / data 136 / bss 0; 77,007 in the latest clean-archive
   run), as expected from the shared core.
   This is **not** a claim of zero overhead versus the macro path: the legacy
   branch already measured that reflection carries a per-type cost.

7. **Diagnostics.** Opted-in types get targeted `static_assert`s for
   union/base/private/reference/const/unsupported members. Reference and const
   members are rejected explicitly rather than failing in the library's
   `get<T&>()` internals.

8. **Wire contract.** Missing key → 403, non-object/array/null input → 304,
   extra keys ignored; a missing key for `std::optional` is 403, not `nullopt`;
   a present `null` still gives `nullopt`.

9. **Complement, not replacement.** `WITH_NAMES`, `WITH_DEFAULT` and
   `ONLY_SERIALIZE` have no reflection-v1 equivalent (`migration_matrix.cpp`),
   so a drop-in replacement would need later slices.

### Proposal: C3 unconstrained as the primary layer

Use the plain functions as the primary layer, and opt a namespace in with one
using-declaration pair:

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/reflection.hpp>   // plain functions, unconstrained

namespace dto
{
struct point { double x, y; };

// one declaration pair per namespace; all reflectable class/union types
// in `dto` are now serializable, and a custom serializer still wins
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
}

nlohmann::json j = dto::point{1.0, 2.0};   // {"x":1.0,"y":2.0}
```

The namespace is the boundary, and that is deliberate:

- the using-declaration must be `using nlohmann::reflection::to_json;` /
  `using nlohmann::reflection::from_json;` — a `using namespace
  nlohmann::reflection;` directive is not found by ADL;
- it must appear in the type's own namespace, before the first use, and in
  every TU;
- it covers complete class/union types in that namespace (a non-inline nested
  namespace needs its own declaration);
- private/protected members, bases, unions, anonymous members, reference/const
  members and unserializable member types are hard errors;
- a fully generic custom `to_json` plus the reflection template is ambiguous;
  a non-template custom serializer is fine.

I recommend a dedicated DTO/transport namespace rather than a general domain
namespace, precisely because the opt-in is namespace-wide.

The per-type alternatives remain available when the namespace is not a good
boundary:

```cpp
// optional safety mode: one type, no `using` required
namespace nlohmann::reflection
{
template<> inline constexpr bool enable<third_party::widget> = true;
}

// optional sugar for an owned type
struct [[=nlohmann::json_serializable{}]] owned { int id{}; };
```

The experiment currently exposes the unconstrained mode behind
`JSON_REFLECTION_NAMESPACE_UNCONSTRAINED`; if this direction is accepted, that
should become the default and the constrained mode can remain as an explicit
safety opt-in. The experiment ships two headers
(`reflection.hpp` + `reflection_namespace.hpp`); they can be merged into one
before the PR.

### Note on the issue body

The original #5959 body is annotation-only and predates this comparison. Two
prototype claims there are now stale: the feature-macro gate and the GCC 16 CI
job are implemented on the feature branch, and the negative-compile harness now
has 13 annotation + 15 namespace cases (including reference/const guards). I can
update the body once the opt-in surface is settled.

### Verification

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

### Questions

1. **Primary opt-in:** is the namespace-wide `using` caveat acceptable as the
   primary surface? If not, C2 remains the fallback.
2. Namespace name: `nlohmann::reflection`?
3. If C2 is kept as a safety mode: `enable<T>` spelling/mechanism, and should it
   stay in the same header?
4. Annotation: keep as sugar, or drop?
5. Header packaging: one `<nlohmann/reflection.hpp>` or two?
6. If the namespace-wide caveat is rejected but delegation is required: should
   we add the no-forwarding `enable<T>` + `using` variant instead?
