// cf_long_name.cpp — negative compile test: a json_name key longer than the
// 63-char structural buffer (char[64] incl. NUL) must fail to compile. The
// json_name consteval constructor turns it into a clear static_assert (rather
// than the old opaque aggregate "initializer-string too long" error).
// EXPECT-DIAG: key too long
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct S
{
    // 64 'a's — one past the 63-char json_name key limit.
    int a [[ = refl2::json_name{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]];
};

int main()
{
    json j;
    S s;
    refl2::codec<false>::to_json(j, s); // must not compile
}
