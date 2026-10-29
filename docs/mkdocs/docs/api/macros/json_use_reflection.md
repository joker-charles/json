# JSON_USE_REFLECTION

!!! warning "Removed"

    **This macro no longer exists on this branch.** It belonged to an earlier
    experiment (`refl2`) that compiled a wide-open reflection catch-all into
    `detail::{to,from}_json`. That experiment was retired and its code removed.

## What to use instead

Use the standalone extension described in
[static reflection](../features/reflection.md):

```cpp
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>

struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};
```

No feature macro is involved: including the header is the opt-in, and
annotating a type is what makes that type participate.

## Why it was withdrawn

- A catch-all changed behavior for types that never asked to participate;
  the replacement only affects types carrying `json_serializable`.
- Its codec reached `basic_json`'s private `json_value` layout through
  `access_context::unchecked()`, which is not a stable contract. The
  replacement builds values only through the public `basic_json` interface.
- It was never installed as part of the library's header set.

The retired code and its experiments are preserved on the
`legacy/reflection-experiments` branch.

```cpp
#define JSON_USE_REFLECTION /* removed; use <nlohmann/reflection.hpp> */
```
