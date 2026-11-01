// cf_ns_unsupported_member.cpp — an enabled type with a member that has no
// public JSON path must get the actionable static_assert, not a deep error.
// EXPECT-DIAG: member is not JSON-serializable
#include <nlohmann/reflection_namespace.hpp>

#include <memory>

struct bad
{
    std::unique_ptr<int> pointer;
    int value{};
};

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<bad> = true;
} // namespace nlohmann::reflection

int main()
{
    nlohmann::json j = bad{}; // must not compile
    (void)j;
}
