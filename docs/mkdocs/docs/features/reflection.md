# Static reflection (experimental)

!!! warning "Experimental"

    Static reflection is an **experimental, opt-in add-on** developed on the
    `feature/static-reflection` branch of the library. It builds on C++26 static
    reflection (P2996) and is **not part of the default library**: it lives in a
    separate header that `<nlohmann/json.hpp>` and the amalgamated single header
    do **not** include. Without that include, nothing changes for anyone.

    The extension is C++26-only and, at the time of writing, has exactly one
    implementation (GCC 16 with `-freflection`). Treat the API as unstable until
    a second implementation exists.

## The header is explicit, and so is every type

The extension is a single add-on header:

```cpp
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>   // not pulled in by json.hpp
```

Including it changes nothing by itself. Participation is **per type**: a class
takes part only if it carries the opt-in annotation

```cpp
struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};
```

Every type without that annotation behaves exactly as it does today — it is not
serializable unless you provide `to_json`/`from_json` or an
[`adl_serializer`](../api/adl_serializer/index.md) specialization.

## Serializing an annotated struct

Once annotated, a flat struct needs no macro and no hand-written serializer:

```cpp
struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};

json j = point {1.0, 2.0};            // {"x":1.0,"y":2.0}
auto p = j.get<point>();              // round-trip
```

The output is byte-identical to what the
[`NLOHMANN_DEFINE_TYPE_*`](../api/macros/nlohmann_define_type_non_intrusive.md)
macro family produces for the same type (asserted by the `unit-static_reflection`
doctest target).

The annotation must follow the `struct`/`class` keyword (`struct [[=...]] S`,
not `[[=...]] struct S`).

## What a member may be

The serializer is a constrained
[`adl_serializer<T, void>`](../api/adl_serializer/index.md) partial
specialization built **only on the public interface** of `basic_json`
(`object()`, `operator[]`, `at()`, `get<T>()`). It never reaches into
`basic_json`'s private layout.

Each member is serialized through the normal path, so a member may be:

- a scalar or string,
- a library type such as `std::vector<point>`, `std::map<std::string, point>`,
  `std::optional<int>` or `std::array<int, 3>`,
- another annotated struct,
- any type with your own ADL `to_json`/`from_json`.

```cpp
struct [[=nlohmann::json_serializable{}]] outer
{
    int id{};
    point p;
    std::vector<point> ps;
    std::optional<int> opt;
};
// {"id":1,"opt":9,"p":{"x":1.0,"y":2.0},"ps":[{"x":3.0,"y":4.0}]}
```

A member whose type has no serialization path is a **compile-time error with an
actionable message**, not a silently omitted key:

```text
error: static assertion failed: json_serializable member is not JSON-serializable;
provide ADL to_json() or annotate the member type
```

This is the property the extension exists for: the
`NLOHMANN_DEFINE_TYPE_*` macro family fails *open* — a member left out of the
macro list is silently dropped and never reported. Reflection sees every member,
so the mistake surfaces at compile time.

## Scope of the first slice

Deliberately small:

- flat structs only: public, non-static data members; no base classes, no
  unions, no private/protected members, no anonymous members (each reported by
  its own `static_assert`);
- no member rename/skip/default annotations, no enum string mapping, no
  `std::variant` handling, no binary formats.

Those are later slices and are not part of this header.

## Not included

The following were explored on this branch but are **not** features of
`<nlohmann/reflection.hpp>`, and are not part of the library at all:

- the `JSON_USE_REFLECTION` wide-open catch-all and the `refl2` codec that
  provided it — retired and removed from this branch, along with its
  `JSON_HAS_CPP_26_REFLECTION` macro;
- annotation-driven member renaming/skipping and enum mapping;
- reflection-driven MessagePack / UBJSON / BSON readers and writers.

They are parked on the `legacy/reflection-experiments` branch, together with
their experiments, and are not part of the proposed upstream surface.

## Limitations

- Requires a P2996 toolchain: GCC 16 with `-std=c++26 -freflection`. On any
  other toolchain the header is **inert** — including it compiles, but
  `nlohmann::json_serializable` does not exist, so annotated code fails at the
  annotation. The C++11 build contract of the main library is untouched.
- The extension is not covered by the amalgamated `single_include` header.
- Experimental: the API may change as further compiler implementations land.

## Further reading

The design and verification material lives in the repository's
`docs/static-reflection/` directory (not part of this website), starting with
`FLAT_STRUCT_REFLECTION.md` and `CORE_CLEANUP_2026-10.md`.
