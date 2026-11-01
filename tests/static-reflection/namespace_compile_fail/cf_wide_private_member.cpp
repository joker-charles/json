// cf_wide_private_member.cpp — in JSON_REFLECTION_NAMESPACE_UNCONSTRAINED
// mode, a namespace `using` alone opts every class in; an unsupported type now
// reaches the serializer body and fails there (the maintainer's caveat).
// EXPECT-DIAG: all non-static data members to be public
#define JSON_REFLECTION_NAMESPACE_UNCONSTRAINED

#include <nlohmann/reflection_namespace.hpp>

namespace dm
{
struct bad
{
    int visible{};

private:
    int hidden{};
};

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace dm

int main()
{
    nlohmann::json j = dm::bad{}; // must not compile
    (void)j;
}
