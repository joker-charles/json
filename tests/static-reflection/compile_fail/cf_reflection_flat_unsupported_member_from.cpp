// cf_reflection_flat_unsupported_member_from.cpp — negative compile test:
// the from_json direction rejects a member with no deserialization path.
// EXPECT-DIAG: member is not JSON-deserializable
#include <nlohmann/reflection.hpp>

#include <memory>

struct [[ = nlohmann::json_serializable {}]] bad
{
    std::unique_ptr<int> pointer;
    int value{};
};

int main()
{
    const nlohmann::json j = nlohmann::json::object();
    (void)j.get<bad>(); // must not compile
}
