// cf_reflection_flat_unannotated.cpp — negative compile test: including the
// standalone flat-struct extension must not make an unannotated struct
// serializable. The error must stay the main library's "non-scalar type"
// conversion diagnostic (zero behavior change for unannotated types).
// EXPECT-DIAG: non-scalar type
#include <nlohmann/reflection.hpp>

struct plain
{
    int value{};
};

int main()
{
    nlohmann::json j = plain{}; // must not compile
    (void)j;
}
