# JSON_USE_REFLECTION

```cpp
#define JSON_USE_REFLECTION /* undefined by default */
```

Opt-in switch for the experimental
[static reflection extension](../features/reflection.md). When defined, and
when the compiler provides C++26 static reflection (P2996), the reflection
catch-all in `detail::{to,from}_json` is compiled in and classes annotated
with `[[=refl2::json_serializable{}]]` gain reflection-driven serialization.

When the macro is **not** defined, the library compiles and behaves
byte-identically to the default build (C++11 contract untouched).

## Requirements

The extension is C++26-only and needs a compiler implementing P2996. At the
time of writing that is GCC 16:

```sh
g++-16 -std=c++26 -freflection
```

Defining the macro on a compiler without reflection support has no effect;
including `reflection_to_json.hpp` directly without the macro fails with a
clear `#error`.

## Notes

!!! warning "Experimental"

    This macro and the extension behind it are experimental work on the
    `feature/static-reflection` branch, not part of the released library.

When active, the header defines
[`JSON_HAS_CPP_26_REFLECTION`](json_has_cpp_26_reflection.md) to `1`; feature
code inside the library is guarded on that macro rather than on
`JSON_USE_REFLECTION` directly.

## Top-level `std::variant` (separate switch)

`JSON_USE_REFLECTION` alone does **not** make `basic_json` constructible from
a `std::variant`. Upstream guarantees that it is not (see
`tests/src/unit-regression2.cpp`, issue #1292), so the oneof wire format
(`{"index":N,"value":…}`) for a *top-level* variant is gated behind a second
macro:

```cpp
#define JSON_USE_REFLECTION
#define JSON_USE_REFLECTION_VARIANT
#include <nlohmann/json.hpp>
```

`std::variant` **members** of annotated structs are unaffected: they are
handled by the reflection codec's dedicated branch, not by the top-level
catch-all, and work with `JSON_USE_REFLECTION` alone.

## Examples

??? example

    ```cpp
    #define JSON_USE_REFLECTION
    #include <nlohmann/json.hpp>

    struct [[=refl2::json_serializable{}]] point
    {
        double x;
        double y;
    };

    int main()
    {
        nlohmann::json j = point {1.0, 2.0}; // {"x":1.0,"y":2.0}
        auto p = j.get<point>();             // round-trip
    }
    ```
