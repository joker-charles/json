// cf_wide_using_after_use.cpp — in unconstrained mode the `using` declaration
// is what makes the functions ADL-visible. It must precede the first use;
// declaring it afterwards does not retroactively fix the instantiation.
// EXPECT-DIAG: non-scalar type
#define JSON_REFLECTION_NAMESPACE_UNCONSTRAINED

#include <nlohmann/reflection_namespace.hpp>

namespace dm
{
struct plain
{
    int x{};
};
} // namespace dm

int main()
{
    nlohmann::json j = dm::plain{}; // instantiated before the using below
    (void)j;
}

namespace dm
{
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace dm
