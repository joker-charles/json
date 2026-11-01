// cf_ns_union.cpp — enable<T> on a union is rejected by the flat-struct guard.
// EXPECT-DIAG: does not support unions
#include <nlohmann/reflection_namespace.hpp>

union bad
{
    int integer;
    float floating;
};

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<bad> = true;
} // namespace nlohmann::reflection

int main()
{
    nlohmann::json j = bad{}; // must not compile
    (void)j;
}
