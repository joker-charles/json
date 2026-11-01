// cf_ns_private_member.cpp — enable<T> opts the type in, but the flat-struct
// guard still rejects private data instead of silently dropping it.
// EXPECT-DIAG: all non-static data members to be public
#include <nlohmann/reflection_namespace.hpp>

struct bad
{
    int visible{};

private:
    int hidden{};
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
