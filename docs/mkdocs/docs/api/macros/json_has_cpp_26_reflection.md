# JSON_HAS_CPP_26_REFLECTION

```cpp
#define JSON_HAS_CPP_26_REFLECTION 1
```

Read-only indicator that the experimental
[static reflection extension](../features/reflection.md) is active. It is
defined to `1` by `reflection_to_json.hpp` when **both** of the following
hold:

- the compiler provides C++26 static reflection (P2996), i.e.
  `__cpp_impl_reflection` and `__cpp_lib_reflection` are defined (GCC 16 with
  `-std=c++26 -freflection`), and
- the user defined [`JSON_USE_REFLECTION`](json_use_reflection.md).

## Notes

!!! warning "Experimental"

    This macro and the extension behind it are experimental work on the
    `feature/static-reflection` branch, not part of the released library.

Users normally do not need this macro; it exists so the library's feature
code (for example the M6 enum string mapping) can be guarded on one name
instead of repeating the compiler-and-opt-in condition. It is never
user-definable: defining it manually has no effect.
