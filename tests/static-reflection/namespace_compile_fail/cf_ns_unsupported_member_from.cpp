// cf_ns_unsupported_member_from.cpp — the from_json direction of the same
// guard.
// EXPECT-DIAG: member is not JSON-deserializable
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
    const nlohmann::json j = nlohmann::json::object();
    (void)j.get<bad>(); // must not compile
}
