// cf_reflection_flat_private_base.cpp — negative compile test: a private base
// is invisible to unprivileged() queries, so the has_inaccessible_bases check
// must reject it instead of silently dropping the inherited members.
// EXPECT-DIAG: does not support base classes
#include <nlohmann/reflection.hpp>

struct base
{
    int base_value{};
};

struct [[ = nlohmann::json_serializable {}]] derived : private base
{
    int derived_value{};
};

int main()
{
    derived value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
