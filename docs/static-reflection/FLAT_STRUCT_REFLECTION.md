# Standalone flat-struct reflection extension (`<nlohmann/reflection.hpp>`)

> **Status:** first slice, feature/static-reflection branch. This document is
> the scope/contract for the standalone extension. It is not the refl2 codec
> (`reflection_to_json.hpp`) and not the rjson mirror (`reflection_json.hpp`).

## 1. Contract

The extension is an **explicit add-on**:

```cpp
#include <nlohmann/reflection.hpp>   // user must include it; json.hpp does not
```

- `<nlohmann/json.hpp>` does **not** include it.
- `single_include/nlohmann/json.hpp` does **not** ship it.
- Not including it changes nothing.
- It does not require, define, or consult `JSON_USE_REFLECTION`. The legacy
  refl2 catch-all remains a separate experiment.

Serialization is **opt-in by type**:

```cpp
struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};

nlohmann::json j = point{1.0, 2.0}; // {"x":1.0,"y":2.0}
auto p = j.get<point>();
```

Every type without the annotation keeps the library's existing behavior
byte-for-byte. On a toolchain without P2996 the header is inert: including it
compiles, but `nlohmann::json_serializable` is not defined, so annotated code
fails at the annotation. This keeps the repository's C++11 per-header
self-containment check green.

## 2. What the first slice supports

| Input | Behavior |
|---|---|
| Annotated flat class/struct with public non-static data members | object with one key per member, in both directions |
| Empty annotated struct | `{}` |
| Nested member with ADL `to_json`/`from_json` | normal library serialization path |
| Nested library type (`std::string`, `std::vector<T>`, `std::array<T,N>`, maps, pairs, ...) | normal library serialization path |
| Nested annotated type | same public path, recursively via its own `adl_serializer` specialization |
| Bit-field with a name | supported through the public read/assignment path |
| Member type without a JSON path | actionable `static_assert` |

The serializer is **flat-struct only**: it reflects the direct public
non-static data members of the annotated type. It does not walk base classes or
recursively reflect unannotated nested structs.

## 3. Rejected by design (first slice)

| Input | Diagnostic |
|---|---|
| union | `json_serializable does not support unions` |
| any base class (public, protected, private, virtual) | `... does not support base classes` |
| private/protected non-static data member | `... requires all non-static data members to be public` |
| anonymous union/struct member (no identifier) | `... does not support anonymous members` |
| member type that is not serializable | `... member is not JSON-serializable; provide ADL to_json() or annotate the member type` |
| member type that is not deserializable | `... member is not JSON-deserializable; provide ADL from_json() or annotate the member type` |

For private data the existing `NLOHMANN_DEFINE_TYPE_INTRUSIVE` macro or a
hand-written serializer remains the right tool. For containers of unannotated
plain structs, annotate/ADL-serialize the element type; the container does not
make its elements reflectable.

## 4. Public-API-only implementation

The extension does **not** reflect `basic_json`'s private `json_value`/`m_data`
union and does **not** call `access_context::unchecked()` on the library
internals. Values are built and read the same way a hand-written serializer
does:

- `to_json`: `BasicJsonType::object()` + `j[key] = value.member`
- `from_json`: `j.at(key).template get<Member>()` + `value.member = ...`

Mechanism: a constrained partial specialization of the public customization
point `nlohmann::adl_serializer<T, void>`:

```cpp
template<typename T>
    requires detail::reflection_flat::is_annotated_v<T>
struct adl_serializer<T, void>
{
    template<typename BasicJsonType>
    static void to_json(BasicJsonType& j, const T& value);

    template<typename BasicJsonType>
    static void from_json(const BasicJsonType& j, T& value);
};
```

`basic_json`'s constructor and `get<T>()` already dispatch through
`JSONSerializer<T>` (default `adl_serializer`), so the extension needs no
change to `detail::{to,from}_json` and no catch-all overload.

### Member annotation detection

```cpp
consteval bool has_json_serializable_annotation(std::meta::info type)
{
    for (const auto annotation : std::meta::annotations_of(type))
    {
        if (std::meta::is_same_type(
                std::meta::remove_cvref(std::meta::type_of(annotation)),
                ^^json_serializable))
        {
            return true;
        }
    }
    return false;
}
```

The annotation must follow the class key: `struct [[=nlohmann::json_serializable{}]] S`
(GCC 16 ignores a leading `[[=...]] struct S` with a warning).

## 5. Object/exception semantics

- `to_json` always produces an object. Keys are the member identifiers; object
  key ordering is the normal `object_t` ordering.
- `from_json` reads with `at`:
  - missing key -> `out_of_range` (id 403);
  - non-object JSON -> `type_error` (id 304);
  - extra keys are ignored.
- No member rename/skip/default annotations in this slice.
- No `_WITH_DEFAULT` equivalent; a missing key is an error.
- `json.get<T>()` requires `T` to be default-constructible (the library's
  existing `get` constraint). `from_json(j, existing_value)` does not.
- Nested members use `get<Member>()`, so a member must be default-constructible
  or provide a returning `from_json` on the from side.

## 6. Gating and relation to the legacy codec

- Active only when `__cpp_impl_reflection` and `__cpp_lib_reflection` are
  defined (g++-16 `-std=c++26 -freflection`).
- Without those macros the header is inert; `nlohmann::json_serializable` is
  not declared.
- `JSON_USE_REFLECTION` (the refl2 catch-all) is independent and remains
  unchanged. Do not annotate a type that already has a hand-written serializer
  or a legacy serializer; for an annotated type the extension's
  `adl_serializer` specialization is used.
- The old refl2 `reflection_to_json.hpp` / rjson `reflection_json.hpp` codecs
  are out of scope. The old internal-layout strategy in
  `UPSTREAM_INTEGRATION_PLAN.md` §2.1/Phase 3 does not apply to this extension.

## 7. Non-goals (later slices)

- member rename/skip/default annotations (`json_name`, `json_ignore`,
  `json_default`);
- enum-level string mapping and variant reflection;
- binary formats (MessagePack, UBJSON/BJData, BSON, CBOR);
- private members, base classes, unions, anonymous members (private-member
  access is planned separately in `PRIVATE_MEMBER_REFLECTION_PLAN.md`);
- recursive reflection of unannotated nested structs;
- custom `basic_json` serializer template parameters;
- a general C++26 CI matrix (the single `test-static_reflection_cpp26`
  doctest target exists; broader CI wiring is a follow-up).

## 8. Tests

C++26 doctest target: `tests/src/unit-static_reflection.cpp`
(`test-static_reflection_cpp26`) — the portable subset of the old catch-all
tests: plain/nested structs, library containers, object-access semantics, and
per-type opt-in.

Runtime fixture: `tests/static-reflection/reflection_flat_struct.cpp`

```sh
# split headers
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o build/scratch/reflection_flat_struct \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct

# amalgamated library + split add-on
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
  -Isingle_include -Iinclude -o build/scratch/reflection_flat_struct_single \
  tests/static-reflection/reflection_flat_struct.cpp
./build/scratch/reflection_flat_struct_single
```

Negative cases live in `tests/static-reflection/compile_fail/`:

- `cf_reflection_flat_unannotated.cpp`
- `cf_reflection_flat_unsupported_member.cpp`
- `cf_reflection_flat_private_member.cpp`
- `cf_reflection_flat_base.cpp`
- `cf_reflection_flat_union.cpp`
- `cf_reflection_flat_anonymous_member.cpp`
- `cf_reflection_flat_not_in_json.cpp` (json.hpp alone must not expose the
  annotation type)

The existing harness auto-discovers all cases:

```sh
GXX=g++-16 bash tests/static-reflection/compile_fail.sh
```

Self-containment checks (header must be inert without P2996):

```sh
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
  g++-16 -std=c++11 -Iinclude -x c++ - -fsyntax-only
printf '#include <nlohmann/reflection.hpp>\nint main(){}\n' | \
  g++-16 -std=c++26 -Iinclude -x c++ - -fsyntax-only
```

No-amalgamation checks:

```sh
! grep -q 'nlohmann/reflection.hpp' include/nlohmann/json.hpp single_include/nlohmann/json.hpp
! grep -q 'is_annotated_v\|has_json_serializable_annotation' \
    single_include/nlohmann/json.hpp
```
