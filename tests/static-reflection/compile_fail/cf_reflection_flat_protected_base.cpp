// cf_reflection_flat_protected_base.cpp — negative compile test: a protected
// base has the same problem as a private one and must be rejected.
// EXPECT-DIAG: does not support base classes
#include <nlohmann/reflection.hpp>

struct base
{
    int base_value{};
};

struct [[ = nlohmann::json_serializable {}]] derived : protected base
{
    int derived_value{};
};

int main()
{
    derived value{};
    nlohmann::json j = value; // must not compile
    (void)j;
}
