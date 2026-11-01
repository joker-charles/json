# Opt-in comparison: type annotation vs reflection-namespace functions

Status: experiment for [issue #5959](https://github.com/nlohmann/json/issues/5959)
and the maintainer's request in
[discussion #5384](https://github.com/nlohmann/json/discussions/5384)
("please compare both opt-in styles in your API issue. Nothing is decided yet.").

This document compares the **opt-in mechanisms** on the same public-API-only
serialization core. It is not a comparison of the retired `refl2` codec: the
legacy branch is used only as evidence for why a catch-all and private-layout
reflection were dropped.

Toolchain: `g++-16 16.2.0`, `-std=c++26 -freflection`, nlohmann/json 3.12.0,
Ubuntu x86-64. All numbers below are single-toolchain measurements; the durable
part is the method and the same-toolchain comparison.

---

## 1. The opt-in routes

### A. Type annotation (current issue #5959)

```cpp
struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};
```

A constrained partial specialization of `adl_serializer<T, void>` is selected
for annotated types. `basic_json`'s constructor and `get<T>()` call it
directly; no ADL and no `using` declaration are involved.

### B1. Per-type `nlohmann::reflection::enable<T>` (Anketell's addendum)

```cpp
namespace third_party
{
struct widget { int id{}; };
} // namespace third_party

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<third_party::widget> = true;
}
```

A constrained `adl_serializer` specialization (in
`reflection_namespace.hpp`) forwards to the plain functions
`nlohmann::reflection::to_json/from_json`. This is the route for a type you do
not own: it needs neither an annotation nor a `using` declaration, and the
third-party namespace does not have to be reopened.

### B2. Namespace-wide `using` (Anketell's first proposal)

```cpp
namespace dm
{
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
}
```

The functions are ordinary templates. The `using`-declaration introduces them
into `dm`, which is an associated namespace of `dm::T`; the library's internal
unqualified `to_json(j, t)` / `from_json(j, t)` find them through ADL.

There are two modes:

- **constrained (default)**: the functions are constrained on
  `is_reflectable_v<T>` (annotation or `enable<T>`). `using` alone is only a
  visibility declaration; it does **not** opt a type in.
- **unconstrained** (`JSON_REFLECTION_NAMESPACE_UNCONSTRAINED`): `using` alone
  opts in every class/union in the namespace. This is the behavior the
  maintainer described as a caveat: unsupported types fail inside the
  serializer body instead of keeping today's "no serialization path" error.

### C. Annotation as optional sugar

Both per-type routes end in an `adl_serializer` specialization, so annotation is
no longer the only entry point. In the current experiment `enable<T>` uses the
forwarding specialization and annotation keeps its existing dedicated
specialization; the serialization behavior is the same (both call the shared
core) and the precedence is tested below.

---

## 2. Results

### 2.1 Opt-in scope and declaration cost

| Criterion | A annotation | B1 `enable<T>` | B2 `using` (constrained) | B2 `using` (unconstrained) |
|---|---|---|---|---|
| Opt-in unit | type | type | none — visibility only | namespace |
| Declarations per type | 1 (on the definition) | 1 (in `nlohmann::reflection`) | 0 (but a type still needs `enable<T>`/annotation) | 0 per type |
| Declarations per namespace | 0 | 0 | 1 `using` | 1 `using` |
| Works for a type you do not own | no | **yes** | only via `enable<T>` | yes, if the namespace can be reopened |
| Works for `std::` or a closed namespace | no | **yes** | only via `enable<T>` | no |
| Type definition depends on nlohmann | yes | **no** | no | no |
| Unannotated/unenabled types unchanged | yes | yes | yes | **no** |
| Unsupported type with opt-in | targeted `static_assert` | targeted `static_assert` | n/a (not an opt-in) | hard error in body |

The decisive result is the "type you do not own" row: `enable<T>` is the only
route that covers both owned and unowned types without touching the type
definition or reopening its namespace. It is one declaration in a serialization
header. `enable<T>` alone is sufficient: the forwarding `adl_serializer`
specialization is selected before ADL, so no `using` declaration is needed
(`reflection_namespace.cpp` asserts a third-party type with neither an
annotation nor a `using`).

### 2.2 Which serializer wins

Verified by `tests/static-reflection/optin_conflicts.cpp`:

| Scenario | Winner | Why |
|---|---|---|
| annotation + custom ADL `to_json` | annotation | `adl_serializer` specialization bypasses ADL |
| `enable<T>` + custom ADL `to_json` | `enable<T>` | forwarding `adl_serializer` specialization bypasses ADL |
| `using` only + custom ADL `to_json` (either mode) | custom | ADL overload resolution prefers the non-template |
| `enable<T>` + `using` + custom ADL | `enable<T>` | the `adl_serializer` specialization is selected first |
| annotation + `enable<T>` + custom ADL | annotation | dedicated annotation specialization; no ambiguity |
| annotation + `NLOHMANN_DEFINE_TYPE_*` (macro lists only `x`) | annotation | annotation sees every member |
| `enable<T>` + `NLOHMANN_DEFINE_TYPE_*` | `enable<T>` | forwarding specialization bypasses the macro's ADL function |
| `using` only + `NLOHMANN_DEFINE_TYPE_*` (either mode) | macro | non-template ADL function wins |
| `using` only + fully generic custom `to_json` (unconstrained) | ambiguous | neither function template is more specialized |
| explicit `adl_serializer<T, void>` + `enable<T>` | explicit specialization | user customization point is most specific |

The asymmetry is the main design decision left for the maintainer:
**annotation and `enable<T>` silently own the type; namespace `using` composes
with custom serializers.** If `enable<T>` becomes the primary route, a
compile-time diagnostic when a custom ADL serializer is also visible would be
safer than silently ignoring it. If composition is more important than the
one-declaration ergonomics, remove the forwarding specialization and require
`using` as well (then the custom serializer wins, at the cost of two
declarations).

The generic-custom row is a `using`-only edge case: a fully generic user
`to_json(BasicJsonType&, const T&)` in the same namespace is not partially
ordered against the reflection template and is ambiguous
(`namespace_compile_fail/cf_wide_generic_ambiguity.cpp`). A non-template custom
serializer is fine.

### 2.3 Namespace scope

- A `using`-declaration is effective for the namespace it appears in. It is
  **not** inherited by a non-inline nested namespace:
  `outer_scope::inner::nested` is not covered by a `using` in `outer_scope`
  (`reflection_namespace.cpp` asserts this).
- The `enable<T>` route does not depend on namespace nesting at all.
- A type in `std` (or another namespace you must not reopen) cannot use the
  `using` route; `enable<T>` and an explicit `adl_serializer` specialization are
  the options.

### 2.4 Visibility and ODR

`using` and `enable<T>` are external declarations, not part of the type
definition. They must be visible **before the first instantiation** that asks
`has_to_json`/`has_from_json`, and consistently in every TU.

`tests/static-reflection/optin_odr/` demonstrates the hazard: `a.cpp` sees
`enable<odr::type>` and asserts `is_constructible_v<json, odr::type> == true`;
`b.cpp` does not see it and asserts the opposite. Both TUs compile. This is an
ODR/IFNDR hazard, not a diagnostic.

Practical rule: put the opt-in (`enable<T>`, `using`, or the annotation) in a
shared header included before any JSON conversion in every TU. Annotation is
structurally safer because it travels with the type definition.

`namespace_compile_fail/cf_ns_enable_after_use.cpp` and
`cf_wide_using_after_use.cpp` lock the "must precede first use" rule.

### 2.5 Diagnostics

For an enabled type with an unsupported member, the `enable<T>` route produces
the same targeted `static_assert` as annotation
(`namespace_compile_fail/cf_ns_unsupported_member.cpp`). The unconstrained
`using` route reaches the same `static_assert` for a private member, but only
after the whole namespace has been opted in — that is the caveat.

`using`-only in the constrained mode keeps the main library's error
(`conversion from 'dm::plain' to non-scalar type ... requested`), which is the
same diagnostic an unannotated type gets today.

### 2.6 Code size and compile time

A 40-type fixture with two members per type was compiled at `-O2 -c` with A and
B1 (`tests/static-reflection/measure_optin.sh`):

| | text | data | bss |
|---|---|---|---|
| annotation | 76,991 | 136 | 0 |
| `enable<T>` | 76,991 | 136 | 0 |

The two objects are the same size, which is expected: both front-ends call the
same `detail::reflection_flat::to_object/from_object` core. A three-run `-O0`
wall-time check was 2.36–2.62 s (A) vs 2.32–2.41 s (B1) — within run-to-run
noise on this machine; no claim of a systematic difference is made.

### 2.7 Boundary evidence

`tests/static-reflection/boundary_matrix.cpp` and the two negative harnesses
cover the shared access/type contract:

| Case | Result |
|---|---|
| named bit-field | supported, round-trips (`{"x":3,"y":7}`) |
| unnamed bit-field | not a subobject, not emitted |
| non-default-constructible | `from_json(existing)` works; `json.get<T>()` keeps the library's default-constructibility rule |
| standalone polymorphic type (no base) | data members only; dynamic type is not part of the format |
| private/protected, any base, union, anonymous member | rejected with the existing targeted `static_assert` |
| reference member | rejected: `does not support reference members; use a value member or a custom serializer` |
| const data member | rejected: `does not support const data members; use a non-const member or a custom serializer` |

The reference/const guards were added after the experiment showed their
`from_json` previously failed with a library-internal error
(`get() cannot be used with reference types`) rather than an actionable
diagnostic. They apply to all three opt-in routes because the core is shared.

### 2.8 Semantics and migration evidence

`tests/static-reflection/semantics_matrix.cpp` locks the wire contract:

| Input | Reflection | Plain macro |
|---|---|---|
| missing scalar / nested key | 403 | 403 |
| missing `optional` key | **403** | 403 |
| present `null` for `optional` | `nullopt` | `nullopt` |
| `null`/wrong scalar type | 302 | 302 |
| wrong container type | 302 | 302 |
| top-level array / null | 304 | 304 |
| extra key | ignored | ignored |

`_WITH_DEFAULT` remains a real gap: missing keys are filled from a default
object there, while reflection v1 throws 403. `tests/static-reflection/
migration_matrix.cpp` also shows `WITH_NAMES` renames keys and
`ONLY_SERIALIZE` omits `from_json`, neither of which reflection v1 can express.

Consequently v1 is a **complement**, not a drop-in replacement; replacement
would require rename/skip/default/one-way and private/INTRUSIVE-equivalent
slices first.

---

## 3. Recommendation

1. **Keep the plain functions as the implementation surface**:
   `nlohmann::reflection::to_json/from_json` are directly callable and are the
   one place that builds/reads values.
2. **Make per-type `enable<T>` the primary opt-in.** It is the only route that
   covers types you do not own, needs one declaration, does not require
   reopening a namespace, and keeps the data definition free of nlohmann.
3. **Keep annotation as optional sugar** for owned types (one line on the
   definition), implemented through the same core. If the maintainer prefers a
   single mechanism, dropping annotation is possible without changing the core.
4. **Do not present namespace-wide `using` as the default opt-in.** In the
   constrained mode it is only a visibility declaration; in the unconstrained
   mode it changes behavior for every type in the namespace and turns
   unsupported types into hard errors. It is useful as an explicit
   "safety off" choice, not as the primary API.
5. **Decide the custom-serializer precedence before upstreaming.** Either
   document "enable/annotation own the type; do not combine with custom
   serializers", or add a compile-time diagnostic. Silently ignoring a
   hand-written `to_json` is the one behavior in this comparison that is hard to
   defend.
6. **Document the ODR/visibility rule** for `enable<T>`/`using`, and recommend
   putting the opt-in in a shared header. Annotation remains the safer choice
   when the type definition is the only shared declaration.

A possible upstream surface:

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/reflection.hpp>   // plain functions + enable<T> + annotation

// owned type, one-line sugar:
struct [[=nlohmann::json_serializable{}]] point { double x, y; };

// unowned type, one-line registration:
namespace nlohmann::reflection
{
template<> inline constexpr bool enable<third_party::widget> = true;
}
```

The namespace-wide `using` and the `JSON_REFLECTION_NAMESPACE_UNCONSTRAINED`
mode can stay experimental until the maintainer decides whether the convenience
is worth the caveat.

---

## 4. Reproduction

```sh
# everything (runtime, precedence, ODR, both negative harnesses)
GXX=g++-16 bash tests/static-reflection/optin_matrix.sh

# just the namespace routes
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
  -Iinclude -o build/scratch/reflection_namespace \
  tests/static-reflection/reflection_namespace.cpp
./build/scratch/reflection_namespace

# precedence matrix
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
  -Iinclude -o build/scratch/optin_conflicts \
  tests/static-reflection/optin_conflicts.cpp
./build/scratch/optin_conflicts

# unconstrained namespace-wide mode
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
  -Iinclude -o build/scratch/optin_namespace_wide \
  tests/static-reflection/optin_namespace_wide.cpp
./build/scratch/optin_namespace_wide

# negative harnesses
GXX=g++-16 bash tests/static-reflection/namespace_compile_fail.sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh

# same-core codegen check (40 types, -O2 object sizes)
GXX=g++-16 bash tests/static-reflection/measure_optin.sh
```

`tests/static-reflection/optin_matrix.sh` also builds the annotation fixture
and the namespace fixture against `single_include` (`-Isingle_include
-Iinclude`), confirming the add-on works alongside the amalgamated core.

---

## 5. Open questions for the API issue

1. Namespace name: `nlohmann::reflection`?
2. Is `enable<T>` the right spelling/mechanism, or should it be a trait class
   or an ADL customization point that does not require opening `nlohmann`?
3. Is annotation kept as sugar or dropped?
4. Should `enable<T>`/annotation refuse to compile when a custom ADL serializer
   is also visible, or is "the opt-in owns the type" the documented contract?
5. Is the namespace-wide `using` (unconstrained mode) wanted at all, or only as
   an experiment?
6. Should the final surface be one header (`<nlohmann/reflection.hpp>`) or two
   (`reflection.hpp` + `reflection_namespace.hpp`)?
