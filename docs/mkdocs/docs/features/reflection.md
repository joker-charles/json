# Static reflection (experimental)

!!! warning "Experimental"

    An **opt-in, per-type add-on** for C++26 static reflection (P2996). It
    lives in a separate header that `<nlohmann/json.hpp>` and the amalgamated
    single header do **not** include, and is enabled by defining
    `JSON_USE_REFLECTION` before including it (it needs `-freflection`). Only
    annotated types are affected; everything else behaves exactly as today.

    GCC 16 is currently the only implementation, so treat the API as unstable
    until a second one lands.

## Opt-in: define the macro, include the header, annotate the type

```cpp
#define JSON_USE_REFLECTION
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>   // not pulled in by json.hpp

struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};

nlohmann::json j = point {1.0, 2.0};   // {"x":1.0,"y":2.0}
const auto p = j.get<point>();         // round-trip
```

`JSON_USE_REFLECTION` must be defined before the header is included; without
it the header is a no-op, so including it by accident changes nothing. With the
macro, a type without the annotation still keeps today's behavior — it is not
serializable unless you provide `to_json`/`from_json` or an
[`adl_serializer`](../api/adl_serializer/index.md) specialization.

The annotation must follow the `struct`/`class` keyword (`struct [[=...]] S`;
a leading `[[=...]] struct S` is ignored by GCC 16 with a warning). The output
is byte-identical to what the
[`NLOHMANN_DEFINE_TYPE_*`](../api/macros/nlohmann_define_type_non_intrusive.md)
macro family produces for the same type.

## Members go through the normal serialization path

The serializer is a SFINAE-constrained
[`adl_serializer<T, void>`](../api/adl_serializer/index.md) partial
specialization (no C++20 concepts layer) built **only on the public interface**
of `basic_json`
(`object()`, `operator[]`, `at()`, `get<T>()`); it never touches
`basic_json`'s private layout. Each member is therefore serialized exactly as a
hand-written serializer would do it:

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

Scalars and strings work, as do library containers, other annotated structs, and
any type with your own ADL `to_json`/`from_json`. A member with no serialization
path is a **compile-time error with an actionable message**, never a silently
omitted key:

```text
error: static assertion failed: json_serializable member is not JSON-serializable;
provide ADL to_json() or annotate the member type
```

That is the property the extension exists for: `NLOHMANN_DEFINE_TYPE_*` fails
*open* — a member left out of the macro list is dropped without a word, whereas
reflection sees every member and the mistake surfaces at compile time.

## Scope

Deliberately small: flat structs only — public, non-static data members, no base
classes, no unions, no private/protected or anonymous members, each rejected by
its own `static_assert`. No member rename/skip/default annotations, no enum
string mapping, no `std::variant` handling, no binary formats.

## Limitations

- `JSON_USE_REFLECTION` is required on top of the explicit include. Without it
  the header is a no-op; with it, a P2996 toolchain is required: GCC 16 with
  `-std=c++26 -freflection`. On any other toolchain, defining the macro is a
  clear `#error` instead of a confusing "annotation not declared" later. The
  C++11 build contract of the library is untouched.
- Not covered by the amalgamated `single_include` header.
