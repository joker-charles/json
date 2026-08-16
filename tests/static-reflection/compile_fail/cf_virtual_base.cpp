// cf_virtual_base.cpp — negative compile test: a struct with a VIRTUAL base
// would flatten the shared virtual subobject multiple times (no dedup), so
// the refl2 codec rejects it. Compilation MUST fail with the dedicated
// static_assert.
// EXPECT-DIAG: virtual base class not supported
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct VB
{
    int vb{};
};
struct V1 : virtual VB
{
    int x1{};
};

int main()
{
    json j;
    V1 v;
    refl2::codec<false>::to_json(j, v); // must not compile
}
