// cf_union.cpp — negative compile test: a UNION must be rejected by the refl2
// codec (unions have no JSON mapping and are deliberately not reflectable).
// Compilation MUST fail with the priority-0 static_assert.
// EXPECT-DIAG: Unions are not supported
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

union U
{
    int i;
    double d;
};

int main()
{
    json j;
    U u{1};
    refl2::codec<false>::to_json(j, u); // must not compile
}
