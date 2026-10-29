// cf_reflection_flat_anonymous_member.cpp — negative compile test: an
// anonymous union/struct member has no JSON key and must be rejected instead
// of silently skipped.
// EXPECT-DIAG: does not support anonymous members
#include <nlohmann/reflection.hpp>

struct [[ = nlohmann::json_serializable {}]] bad
{
    union
    {
        int integer;
        float floating;
    };

    int value{};
};

int main()
{
    bad value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
