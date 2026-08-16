// cf_json_name_collision.cpp — negative compile test: two members annotated
// with the SAME json_name key would produce colliding JSON keys, so the refl2
// codec rejects it. Compilation MUST fail with the dedicated static_assert.
// EXPECT-DIAG: duplicate JSON keys
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct S
{
    int a [[=refl2::json_name{"k"}]];
    int b [[=refl2::json_name{"k"}]];
};

int main()
{
    json j;
    S s;
    refl2::codec<false>::to_json(j, s); // must not compile
}
