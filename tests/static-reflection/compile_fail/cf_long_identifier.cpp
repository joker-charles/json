// cf_long_identifier.cpp — negative compile test: a member IDENTIFIER longer
// than the 63-char key buffer must fail to compile. Previously the identifier
// path copied a full 64 bytes into the zero-initialized array with no room for
// the NUL terminator, so `std::string(key.data())` read past the array and
// produced a garbage JSON key (observed: the key leaked the following
// std::string's heap bytes). The consteval length check turns it into a clear
// diagnostic instead.
// EXPECT-DIAG: identifier too long
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct [[ = refl2::json_serializable {}]] S
{
    // 64 'a's — one past the 63-char identifier limit.
    int aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;
};

int main()
{
    json j;
    S s{};
    refl2::codec<false>::to_json(j, s); // must not compile
}
