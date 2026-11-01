// cf_ns_const_member.cpp — the namespace front-end shares the same core, so a
// const member gets the same actionable rejection as annotation.
// EXPECT-DIAG: does not support const data members
#include <nlohmann/reflection_namespace.hpp>

struct bad
{
    const int x;
};

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<bad> = true;
} // namespace nlohmann::reflection

int main()
{
    bad b{1};
    nlohmann::json j = b; // must not compile
    (void)j;
}
