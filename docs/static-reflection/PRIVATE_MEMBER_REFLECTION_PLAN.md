# Next slice: explicit private-member reflection

> **Status: plan, not implemented.** The current first slice of
> `<nlohmann/reflection.hpp>` reflects only public non-static data members and
> rejects private/protected members with a static_assert. This document is the
> plan for an explicit, per-type private-member opt-in. It does **not** change
> the rule that `basic_json`'s private layout is off limits.

## 1. Problem

A class with private data members cannot use
`[[=nlohmann::json_serializable{}]]` today. The only ways to serialize it are
the existing `NLOHMANN_DEFINE_TYPE_INTRUSIVE` family or a hand-written
`to_json`/`from_json`. That is deliberate: silently skipping private members
would lose data, so the first slice rejects the type instead.

The question for the next slice is whether reflection can support private
members **without** breaking encapsulation implicitly and **without** becoming
a consumer of `basic_json` internals.

## 2. Two different access problems

| Layer | Rule |
|---|---|
| `basic_json`'s private layout (`json_value` / `m_data`) | **Never** reflected. Values are built/read through the public API (`object()`, `operator[]`, `at()`, `get<T>()`), exactly as a hand-written serializer. |
| The user's own type's private members | Can be supported only through an **explicit per-type opt-in**; reflection is allowed to see them only after the user asks for it. |

The quoted review rule ("do not reflect into `basic_json`'s private members via
`unchecked()`") targets the first row. The second row is a separate design
axis. This slice must keep the first row untouched.

If the project's rule is interpreted even more strictly as "no `unchecked()`
anywhere", this slice is not implementable by reflection; private types must
continue to use the INTRUSIVE macros or hand-written serializers.

## 3. Current behavior (baseline to preserve)

`reflection.hpp` currently:

- selects types with `detail::reflection_flat::is_annotated_v<T>`;
- enumerates members with
  `std::meta::nonstatic_data_members_of(^^T, access_context::unprivileged())`;
- rejects inaccessible members in `validate_flat_struct<T>()`:
  ```cpp
  static_assert(!std::meta::has_inaccessible_nonstatic_data_members(
                    ^^T, std::meta::access_context::unprivileged()),
                "json_serializable requires all non-static data members to be public");
  ```
- builds values with `BasicJsonType::object()` / `operator[]` and reads with
  `at()` / `get<T>()`.

The negative side is pinned by
`tests/static-reflection/compile_fail/cf_reflection_flat_private_member.cpp`.

## 4. Proposed public API

Add a separate marker type rather than overloading `json_serializable`:

```cpp
struct [[=nlohmann::json_serializable_private{}]] secretive
{
private:
    int hidden{};
public:
    int visible{};
};
```

Rules:

- `json_serializable` keeps its current meaning: public members only.
- `json_serializable_private` means: this type explicitly allows the extension
  to read/write its private and protected non-static data members.
- Both markers on the same type is a compile error (ambiguous intent).
- The marker has no effect on non-class types; the annotation type is only
  selected for classes/unions through `is_annotated_v`-style detection.
- Annotation placement follows the existing rule: after the `struct`/`class`/
  `union` keyword.

Why a separate marker instead of a flag on `json_serializable`:

- the two policies are easy to tell apart in diagnostics and docs;
- `json_serializable{}` remains a plain empty structural marker;
- adding a stored `bool`/enum to the annotation type changes its ABI and makes
  every existing annotation slightly more complex;
- a future `json_serializable{.include_private=true}` form can be added as an
  alias later without breaking either marker.

## 5. Access policy

| Type annotation | Reflection access context | `basic_json` access |
|---|---|---|
| none | no reflection | normal library behavior |
| `json_serializable` | `access_context::unprivileged()` | public API only |
| `json_serializable_private` | `access_context::unchecked()` on the user type | public API only |

The serializer must not use `unchecked()` on `basic_json` itself. The only
`unchecked()` use in the proposed slice is on the user's type, after the marker
has been seen at compile time.

## 6. Serializer sketch

The change is intentionally small and localized:

1. Add `struct json_serializable_private {};` next to `json_serializable`.
2. Replace the boolean `is_annotated_v` with an access-mode selector, e.g.
   ```cpp
   enum class access_mode { none, public_only, include_private };
   ```
   or a pair of `consteval` helpers:
   ```cpp
   consteval bool has_public_marker(std::meta::info type);
   consteval bool has_private_marker(std::meta::info type);
   ```
3. Select the context:
   ```cpp
   consteval std::meta::access_context access_context_for(bool include_private)
   {
       return include_private
              ? std::meta::access_context::unchecked()
              : std::meta::access_context::unprivileged();
   }
   ```
   Use the selected context both for `nonstatic_data_members_of` and for the
   class-level accessibility guard.
4. Make the "all members public" static_assert conditional:
   - public-only mode keeps the current static_assert;
   - private mode skips it, but still rejects inaccessible bases, unions, and
     anonymous members.
5. Keep the value-side implementation exactly as it is today:
   `object()`, `operator[]`, `at()`, `get<T>()`, assignment.
6. Keep the generated object keys as the member identifiers; this slice is
   about access only, not about member rename/skip/default annotations.

Expected header growth: roughly 30-60 lines, depending on how the access-mode
selection is written. No recursive codec, no private `basic_json` access.

## 7. Semantics

- Same flat-struct semantics as the first slice: object with one key per
  serialized non-static data member; missing key throws 403; non-object JSON
  throws 304; extra keys are ignored.
- Private/protected members are treated exactly like public members once the
  marker is present.
- `json.get<T>()` still requires `T` to be default-constructible; a private
  default constructor/private factory is a C++ access problem, not a
  reflection problem. `from_json(j, existing)` can still be used when the
  caller has access to an existing object.
- Base classes stay rejected, including private/protected/virtual bases:
  flattening inheritance is a separate feature and would reintroduce the old
  codec's complexity.
- Unions and anonymous members stay rejected.
- Member rename/skip/default annotations (`json_name`, `json_ignore`,
  `json_default`) are **not** part of this slice.

## 8. Diagnostics

| Situation | Expected diagnostic |
|---|---|
| private member, no marker | current `json_serializable requires all non-static data members to be public` |
| private member, `json_serializable_private` | compiles |
| both markers | static_assert: choose exactly one access policy |
| marker on union | existing union static_assert |
| marker on type with a base | existing base-class static_assert |
| marker on type with an anonymous member | existing anonymous-member static_assert |
| marker on a non-class type | marker is simply not selected; class/enum behavior is unchanged |

The "both markers" diagnostic is new and must be tested.

## 9. Tests

Runtime:

- private-only struct round-trips with `json_serializable_private`;
- mixed public/private struct round-trips;
- private nested member whose type is also annotated;
- `ordered_json` works through the same public API;
- output is byte-identical to the `NLOHMANN_DEFINE_TYPE_INTRUSIVE` baseline for
  the same struct.

Compile-fail:

- private member + `json_serializable` still fails with the current message;
- both annotations on one type fails with the new message;
- private marker + base class still fails;
- private marker + union still fails;
- private marker + anonymous member still fails;
- an unannotated private type remains not serializable (baseline).

Audit:

- `grep` for `unchecked`/`json_value`/`m_data` in `reflection.hpp` must find
  only the user-type access context and comments, never `basic_json` internals;
- the existing public-API fixture and container tests must keep passing.

## 10. Compatibility and precedence

The current first slice gives the annotation path precedence over a
hand-written ADL serializer for an annotated type. That means:

- do not annotate a type that already has a hand-written serializer or an
  explicit `adl_serializer<T, void>` specialization;
- do not annotate a type that uses `NLOHMANN_DEFINE_TYPE_INTRUSIVE`; use one
  mechanism or the other;
- this slice does not add automatic precedence detection. If that becomes a
  requirement, it is a separate change (and brings back the old codec's
  non-circular ADL-probe complexity).

## 11. Acceptance criteria

1. Core headers and `single_include` remain `develop`-clean.
2. `basic_json` is still touched only through the public API; no
   `json_value`/`m_data` access and no `unchecked()` on `basic_json`.
3. Without the private marker, a type with private members still fails to
   compile with an actionable message.
4. With the private marker, a flat struct with private/protected members
   round-trips and matches the INTRUSIVE-macro baseline.
5. Base/union/anonymous-member rejection is unchanged.
6. The existing flat-struct tests, compile-fail harness, and the C++26 doctest
   target stay green.
7. The header stays small enough to review as a single slice; no member
   annotation layer is added.

## 12. Non-goals

- `json_name` / `json_ignore` / `json_default` member annotations;
- private/protected/virtual base flattening;
- enum/variant/binary reflection;
- automatic private access without an explicit marker;
- reflection into `basic_json` internals;
- custom `basic_json` serializer template parameters.

## 13. Open questions

1. Marker spelling: `json_serializable_private` (recommended) vs
   `json_serializable{.include_private=true}` vs a nested namespace such as
   `nlohmann::detail`-private marker. Public marker naming is an API decision.
2. Is `unchecked()` on the user type acceptable to the project, or should
   private access require a friend/accessor declared by the user? The quoted
   no-go only forbids `basic_json` internals, but some reviewers may want zero
   `unchecked()` anywhere.
3. Should the private marker be allowed on types that also have a hand-written
   serializer? Current recommendation: no, keep "one mechanism per type" and
   document it.
4. Does the slice need a separate test target, or should it extend
   `tests/src/unit-static_reflection.cpp` and the compile-fail harness only?

## 14. Rollout

1. Land this plan document.
2. Add the marker and access-mode plumbing to `reflection.hpp`.
3. Add runtime + compile-fail tests and the INTRUSIVE-macro parity check.
4. Update `FLAT_STRUCT_REFLECTION.md`, `CORE_CLEANUP_2026-10.md` (if needed),
   `tests/static-reflection/README.md`, and the branch guide.
5. Keep commits split: add-on header / tests / docs, each DCO-signed.

No implementation is part of the current clean-core state; this is a planned
next slice.
