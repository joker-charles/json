// cf_ns_enable_after_use.cpp — enable<T> must be visible before the first
// instantiation that asks has_to_json<json, T>; a later specialization cannot
// retroactively make the earlier use compile.
// EXPECT-DIAG: non-scalar type
#include <nlohmann/reflection_namespace.hpp>

namespace dm
{
struct type
{
    int x{};
};
} // namespace dm

int main()
{
    nlohmann::json j = dm::type{}; // instantiated before the enable<T> below
    (void)j;
}

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<dm::type> = true;
} // namespace nlohmann::reflection
