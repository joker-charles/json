// cf_ns_base.cpp — enable<T> does not silently flatten base subobjects; the
// same guard as the annotation route must fire.
// EXPECT-DIAG: does not support base classes
#include <nlohmann/reflection_namespace.hpp>

struct base
{
    int base_value{};
};

struct derived : base
{
    int derived_value{};
};

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<derived> = true;
} // namespace nlohmann::reflection

int main()
{
    nlohmann::json j = derived{}; // must not compile
    (void)j;
}
