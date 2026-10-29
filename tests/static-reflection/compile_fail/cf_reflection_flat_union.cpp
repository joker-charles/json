// cf_reflection_flat_union.cpp — negative compile test: a union has no single
// active-member mapping here; the first slice rejects it explicitly.
// EXPECT-DIAG: does not support unions
#include <nlohmann/reflection.hpp>

union [[ = nlohmann::json_serializable {}]] bad
{
    int integer;
    float floating;
};

int main()
{
    bad value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
