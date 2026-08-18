# Static reflection (experimental)

!!! warning "Experimental"

    Static reflection support is an **experimental, opt-in extension** developed on the
    `feature/static-reflection` branch of the library. It builds on C++26 static
    reflection (P2996) and is not part of the default library: **nothing changes**
    unless you explicitly define [`JSON_USE_REFLECTION`](../api/macros/json_use_reflection.md),
    and even then only the classes you annotate are affected.

## Why opt-in

Making "any reflectable struct serializes automatically" the default would be a
behavior change for existing user code. The extension therefore follows the
feature-macro pattern of the project: the reflection path is compiled in only
when the user asks for it, every behavior change is scoped to explicitly
annotated types, and the unannotated library behaves exactly as before.

## Enabling

The extension requires a compiler with P2996 support (at the time of writing:
GCC 16 with `-std=c++26 -freflection`) and the opt-in macro:

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/json.hpp>
```

Without the macro (or without a P2996 compiler), the library compiles and
behaves byte-identically to the default build.

## Serializing annotated structs

Annotate a class with `[[=refl2::json_serializable{}]]` **after** the
`struct`/`class` keyword to give it reflection-driven serialization:

```cpp
struct [[=refl2::json_serializable{}]] point
{
    double x;
    double y;
};

json j = point {1.0, 2.0};            // {"x":1.0,"y":2.0}
auto p = j.get<point>();              // round-trip
```

No macro, no hand-written `to_json`/`from_json`, and no
[`adl_serializer`](../api/adl_serializer/index.md) specialization needed. The
output is byte-identical to what the
[`NLOHMANN_DEFINE_TYPE_*`](../api/macros/nlohmann_define_type_non_intrusive.md)
macro family produces for the same type (differentially tested).

## Annotations

| Annotation | Level | Semantics |
|---|---|---|
| `[[=refl2::json_serializable{}]]` | type | opt the class into the reflection path (the only way a class participates) |
| `[[=refl2::json_name{"key"}]]` | member | override the JSON key (the `_WITH_NAMES` macro semantics) |
| `[[=refl2::json_ignore{}]]` | member | exclude the member from both directions |
| `[[=refl2::json_default{}]]` | member | `from_json` falls back to `T{}.member` when the key is missing (the `_WITH_DEFAULT` macro semantics) |

```cpp
struct [[=refl2::json_serializable{}]] person
{
    std::string name;
    int age [[=refl2::json_name{"years"}]];
    std::string ssn [[=refl2::json_ignore{}]];
    int score [[=refl2::json_default{}]];
};
```

Annotations are P3394R4 value annotations. The type-level annotation must
follow the `struct` keyword (`struct [[=...]] S`, not `[[=...]] struct S`).

## Enum string mapping

An enum whose enumerators carry `[[=refl2::json_name{"..."}]]` annotations is
mapped to strings, replacing
[`NLOHMANN_JSON_SERIALIZE_ENUM`](../api/macros/nlohmann_json_serialize_enum.md):

```cpp
enum class color
{
    red [[=refl2::json_name{"RED"}]],
    green [[=refl2::json_name{"GREEN"}]]
};

json j = color::green;                // "GREEN"
auto c = j.get<color>();
```

An unannotated enum keeps the integer path byte-for-byte.

## std::variant

A top-level `std::variant` is supported out of the box (no annotation possible
on library types) and serializes as the oneof wire format
`{"index": <active alternative index>, "value": <alternative>}`:

```cpp
std::variant<int, std::string> v {42};
json j = v;                           // {"index":0,"value":42}
auto back = j.get<decltype(v)>();
```

## Limitations

- C++26 static reflection is required; the extension compiles only on
  toolchains providing P2996 (GCC 16+ with `-freflection`).
- Unions, pointers/self-referential types, and non-default-constructible
  types on the `from_json` side are not supported (compile-time errors with
  actionable messages).
- Private/protected members are only serialized under the explicit
  unchecked codec (`refl2::codec<true>`), mirroring the `_INTRUSIVE` macro's
  friend privilege; the default path uses unprivileged reflection (public
  members only).
- The extension relies on the library's internal `basic_json::json_value`
  layout via `access_context::unchecked()` — see the branch's design docs for
  the stability rationale.

## Further reading

The design, verification methodology, and milestone reports live in the
repository's `docs/static-reflection/` directory (not part of this website):
`FEASIBILITY.md`, `M5_REFLECTION_TO_JSON.md`, `EVALUATION.md`, and
`UPSTREAM_INTEGRATION_PLAN.md`.
