// cf_ns_reference_member.cpp — the namespace front-end shares the same core,
// so a reference member gets the same actionable rejection as annotation.
// EXPECT-DIAG: does not support reference members
#include <nlohmann/reflection_namespace.hpp>

struct bad
{
    int& r;
};

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<bad> = true;
} // namespace nlohmann::reflection

int main()
{
    int value = 1;
    bad b{value};
    nlohmann::json j = b; // must not compile
    (void)j;
}
