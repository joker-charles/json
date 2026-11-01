# The extension: `<nlohmann/reflection.hpp>`

Contract for the annotation front-end on this branch. Build and test commands
are in [`README.md`](README.md); compiler facts in
[`VERIFIED_FACTS.md`](VERIFIED_FACTS.md).

> **Experiment-branch note.** `reflection-opt-in-compare` adds
> `<nlohmann/reflection_namespace.hpp>`, which keeps this contract's
> public-API-only core and adds `nlohmann::reflection::enable<T>` and namespace
> `using` as alternative opt-in routes (annotation becomes optional sugar). The
> comparison is in [`OPT_IN_COMPARISON.md`](OPT_IN_COMPARISON.md); the
> annotation contract below is unchanged.

## Opt-in: macro, header, toolchain, annotation

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>   // json.hpp does not include it

struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};

json j = point {1.0, 2.0};           // {"x":1.0,"y":2.0}
auto p = j.get<point>();             // round-trip
```

- **Macro opt-in**: `JSON_USE_REFLECTION` must be defined before the header is
  included. It is a presence switch: any definition enables the extension;
  define it in exactly one place (source or command line). Without it the header
  is a no-op — the annotation type and the serializer do not exist, and an
  accidental include changes nothing.
- **Header opt-in**: not included by `json.hpp`, not amalgamated into
  `single_include`.
- **Type opt-in**: only annotated classes participate. Unannotated types keep
  exactly today's behavior (no `to_json` → compile error).
- **Toolchain opt-in**: with the macro defined, a P2996 toolchain is required
  (`__cpp_impl_reflection` / `__cpp_lib_reflection`, GCC 16 with
  `-std=c++26 -freflection`); otherwise the header fails with a clear `#error`.
  The macro is consulted only by this header; no core header reads it.
- The annotation must follow the class key — `struct [[=...]] S`; a leading
  `[[=...]] struct S` is ignored by GCC 16 with a warning.

## What serializes

| Input | Result |
|---|---|
| annotated class/struct with public non-static data members | object, one key per member, both directions |
| empty annotated struct | `{}` |
| member with ADL `to_json`/`from_json`, or a nested annotated type | normal library path |
| member that is a library type (`std::string`, `std::vector<T>`, `std::array<T,N>`, maps, pairs, …) | normal library path |
| named bit-field | supported through the public read/assign path |

Flat structs only: direct public non-static data members. Base classes are not
flattened, and unannotated nested structs are not reflected — annotate the
element type instead.

## Rejected, with an actionable diagnostic

| Input | `static_assert` message |
|---|---|
| union | `json_serializable does not support unions` |
| any base class (public/protected/private/virtual) | `… does not support base classes` |
| private/protected data member | `… requires all non-static data members to be public` |
| anonymous member (no identifier) | `… does not support anonymous members` |
| member type not serializable | `… member is not JSON-serializable; provide ADL to_json() or annotate the member type` |
| member type not deserializable | `… member is not JSON-deserializable; provide ADL from_json() or annotate the member type` |

This is the property the extension exists for: the `NLOHMANN_DEFINE_TYPE_*`
macros fail *open* (a member left out of the list is silently dropped);
reflection sees every member, so the mistake surfaces at compile time.

For private data the `NLOHMANN_DEFINE_TYPE_INTRUSIVE` macro or a hand-written
serializer remains the right tool.

## Implementation: public API only

A SFINAE-constrained partial specialization of the public customization point
`nlohmann::adl_serializer<T, void>` (no C++20 concepts layer):

```cpp
template<typename T>
struct adl_serializer<T, std::enable_if_t<detail::reflection_flat::is_annotated_v<T>>>
{
    template<typename BasicJsonType>
    static void to_json(BasicJsonType& j, const T& value);      // object() + j[key] = m
    template<typename BasicJsonType>
    static void from_json(const BasicJsonType& j, T& value);    // j.at(key).get<M>()
};
```

`basic_json`'s constructor and `get<T>()` already dispatch through
`JSONSerializer<T>`, so no change to `detail::{to,from}_json` and no catch-all
overload is needed. The extension never reads `basic_json`'s private
`json_value`/`m_data` and never calls `access_context::unchecked()` on library
internals; annotation detection uses only query-domain reflection
(`annotations_of` + `remove_cvref` + `is_same_type`).

## Semantics

- `to_json` always produces an object; keys are member identifiers; ordering is
  the normal `object_t` ordering.
- `from_json` uses `at`: missing key → `out_of_range` (403), non-object JSON →
  `type_error` (304), extra keys ignored.
- No rename/skip/default annotations in this slice; a missing key is an error.
- `json.get<T>()` requires `T` default-constructible (the library's existing
  constraint); `from_json(j, existing_value)` does not.
- Nested members go through `get<Member>()`, so they must be
  default-constructible or provide a returning `from_json`.

## Later slices (not implemented)

- **Private members** — a second marker `json_serializable_private` on the type
  would allow `access_context::unchecked()` **on the user's type** (never on
  `basic_json`), public API still used to build values; both markers on one type
  is a compile error; without the marker, private members keep failing with the
  diagnostic above.
- Member rename/skip/default annotations, enum mapping, `std::variant`, binary
  formats. All were prototyped in the retired work and must be re-done on top of
  this public-API-only base, one reviewable slice at a time.

## Tests

| Artifact | Covers |
|---|---|
| `tests/src/unit-static_reflection.cpp` (`test-static_reflection_cpp26`) | doctest: plain/nested structs, containers, object semantics, per-type opt-in |
| `tests/static-reflection/reflection_flat_struct.cpp` | runtime fixture, split and `single_include` layouts |
| `tests/static-reflection/compile_fail/` + `compile_fail.sh` | the rejection guards above in **both directions**, the add-on-not-included case and the macro-without-P2996 #error (10 TUs) |
| `tests/static-reflection/check_docs_examples.sh` | keeps the mkdocs page's claims executable |

Non-regression that is part of the contract: `include/nlohmann/detail/**` and
`single_include/nlohmann/json.hpp` stay byte-identical to `develop`, and the
header compiles under `-std=c++11` through `-std=c++20` where it is inert.
