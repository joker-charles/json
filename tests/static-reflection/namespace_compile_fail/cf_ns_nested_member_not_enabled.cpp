// cf_ns_nested_member_not_enabled.cpp — enabling the outer type is not enough:
// a member type in another namespace must be enabled (or annotated) itself.
// EXPECT-DIAG: member is not JSON-serializable
#include <nlohmann/reflection_namespace.hpp>

namespace inner
{
struct widget
{
    int id{};
};
} // namespace inner

namespace outer
{
struct holder
{
    inner::widget w{};
};
} // namespace outer

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<outer::holder> = true;
} // namespace nlohmann::reflection

int main()
{
    nlohmann::json j = outer::holder{}; // must not compile
    (void)j;
}
