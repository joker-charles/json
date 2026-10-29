# JSON_HAS_CPP_26_REFLECTION

!!! warning "Removed"

    **This macro is not defined on this branch.** It belonged to an earlier
    experiment (`refl2`) that has been retired and removed.

## What to use instead

See [static reflection](../features/reflection.md). The supported extension
does not need a feature macro: including `<nlohmann/reflection.hpp>` is the
opt-in, and only annotated types are affected.

## Why it was withdrawn

The retired experiment defined `JSON_HAS_CPP_26_REFLECTION` to `1` at the end
of its codec header once the compiler provided P2996 and the user had defined
`JSON_USE_REFLECTION`. It existed only so that reflection-gated code inside
`detail/conversions/{to,from}_json.hpp` could be guarded on one name.

Both the macro and that gated code were removed, and the core conversion
headers are back on the `develop` baseline, so nothing defines or consumes this
name today. The retired code and its experiments are preserved on the
`legacy/reflection-experiments` branch.

```cpp
#define JSON_HAS_CPP_26_REFLECTION /* removed; no longer defined */
```
