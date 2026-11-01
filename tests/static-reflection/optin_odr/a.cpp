// optin_odr/a.cpp — this TU sees enable<odr::type>.
#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include "type.hpp"

#include <type_traits>

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<odr::type> = true;
} // namespace nlohmann::reflection

static_assert(std::is_constructible_v<nlohmann::json, odr::type>,
              "a.cpp sees the enable<T> opt-in");

nlohmann::json make_a()
{
    return odr::type{1};
}
