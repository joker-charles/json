// optin_odr/type.hpp — a type whose opt-in is intentionally NOT in this header.
//
// The opt-in (enable<T> or a namespace `using`) is an external declaration, so
// each TU must see it before its first use. a.cpp sees it; b.cpp does not. Both
// compile, but their is_constructible<json, type> answers differ, which is the
// ODR/IFNDR hazard this fixture demonstrates. Real code must put the opt-in in
// a shared header included by every TU (and before any json conversion).
#pragma once

namespace odr
{
struct type
{
    int x{};
};
} // namespace odr
