// cf_ns_using_only.cpp — a namespace `using` alone is NOT an opt-in in the
// default (constrained) mode; the type must keep today's "non-scalar type"
// conversion error.
// EXPECT-DIAG: non-scalar type
#include <nlohmann/reflection_namespace.hpp>

namespace dm
{
struct plain
{
    int x{};
};
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace dm

int main()
{
    nlohmann::json j = dm::plain{}; // must not compile
    (void)j;
}
