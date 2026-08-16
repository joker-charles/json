// cf_duplicate_member.cpp — negative compile test: two base classes with the
// same member name would produce colliding JSON keys (silent overwrite), so
// the refl2 codec rejects it. Compilation MUST fail with the dedicated
// static_assert.
// EXPECT-DIAG: duplicate JSON keys
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct C1
{
    int x{};
};
struct C2
{
    int x{};
};
struct D : C1, C2
{
    int y{};
};

int main()
{
    json j;
    D d;
    refl2::codec<false>::to_json(j, d); // must not compile
}
