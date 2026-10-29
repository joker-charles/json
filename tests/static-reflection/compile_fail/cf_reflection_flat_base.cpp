// cf_reflection_flat_base.cpp — negative compile test: the flat-struct
// serializer deliberately ignores base subobjects, so a type with a base must
// be rejected instead of silently losing the base members.
// EXPECT-DIAG: does not support base classes
#include <nlohmann/reflection.hpp>

struct base
{
    int base_value{};
};

struct [[ = nlohmann::json_serializable {}]] derived :
base
{
    int derived_value{};
};

int main()
{
    derived value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
