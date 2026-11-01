// cf_reflection_flat_const_member.cpp — the reflection serializer is
// bidirectional; a const data member cannot be assigned in from_json, so it is
// rejected up front with an actionable diagnostic.
// EXPECT-DIAG: does not support const data members
#include <nlohmann/reflection.hpp>

struct [[=nlohmann::json_serializable{}]] bad
{
    const int x;
};

int main()
{
    bad b{1};
    nlohmann::json j = b; // must not compile
    (void)j;
}
