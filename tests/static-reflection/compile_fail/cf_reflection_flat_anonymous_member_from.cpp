// cf_reflection_flat_anonymous_member_from.cpp — negative compile test: the
// from_json direction rejects members without an identifier.
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
    const nlohmann::json j = nlohmann::json::object();
    (void)j.get<bad>(); // must not compile
}
