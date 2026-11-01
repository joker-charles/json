// cf_wide_generic_ambiguity.cpp — an unconstrained user-wide generic to_json in
// the same namespace is not partially ordered against the reflection generic;
// the call is ambiguous. A generic catch-all is therefore not composable with
// the namespace-wide route (a non-template custom serializer still wins).
// EXPECT-DIAG: ambiguous
#define JSON_REFLECTION_NAMESPACE_UNCONSTRAINED

#include <nlohmann/reflection_namespace.hpp>

namespace dm
{
struct type
{
    int x{};
};

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;

template<typename BasicJsonType, typename T>
void to_json(BasicJsonType& j, const T&)
{
    j = 0;
}
} // namespace dm

int main()
{
    nlohmann::json j = dm::type{}; // must not compile: ambiguous
    (void)j;
}
