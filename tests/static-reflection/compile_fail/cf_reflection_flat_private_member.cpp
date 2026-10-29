// cf_reflection_flat_private_member.cpp — negative compile test: the first
// slice only reflects public non-static data members; a private data member
// must be rejected instead of silently skipped.
// EXPECT-DIAG: all non-static data members to be public
#include <nlohmann/reflection.hpp>

struct [[ = nlohmann::json_serializable {}]] bad
{
    int visible{};

private:
    int hidden{};
};

int main()
{
    bad value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
