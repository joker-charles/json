// cf_protected_base.cpp — negative compile test: a struct with a PROTECTED
// base under the unprivileged codec would silently drop the base members, so
// the refl2 codec rejects it. Compilation MUST fail with the dedicated
// static_assert (distinct from the private-base message).
// EXPECT-DIAG: protected base class
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct B
{
    int b{};
};
struct D : protected B
{
    int d{};
    D(int b_, int d_) : B{b_}, d(d_) {}
};

int main()
{
    json j;
    D d(1, 2);
    refl2::codec<false>::to_json(j, d); // must not compile
}
