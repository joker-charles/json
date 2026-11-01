// optin_odr/b.cpp — this TU does NOT see enable<odr::type>.
#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include "type.hpp"

#include <type_traits>

static_assert(!std::is_constructible_v<nlohmann::json, odr::type>,
              "b.cpp does not see the enable<T> opt-in");

nlohmann::json make_b()
{
    return nlohmann::json(0);
}
