// cf_reflection_flat_unsupported_member.cpp — negative compile test: a member
// whose type has no normal JSON serialization path must be rejected with the
// extension's actionable static_assert, not a deep template error.
// EXPECT-DIAG: member is not JSON-serializable
#include <nlohmann/reflection.hpp>

#include <memory>

struct [[ = nlohmann::json_serializable {}]] bad
{
    std::unique_ptr<int> pointer;
    int value{};
};

int main()
{
    nlohmann::json j = bad{}; // must not compile
    (void)j;
}
