// cf_reflection_flat_reference_member.cpp — a reference data member cannot be
// read back through get<member_type>() (the library rejects get<T&>), so the
// extension must reject it with an actionable diagnostic instead of surfacing
// a library-internal static_assert.
// EXPECT-DIAG: does not support reference members
#include <nlohmann/reflection.hpp>

struct [[=nlohmann::json_serializable{}]] bad
{
    int& r;
};

int main()
{
    int value = 1;
    bad b{value};
    nlohmann::json j = b; // must not compile
    (void)j;
}
