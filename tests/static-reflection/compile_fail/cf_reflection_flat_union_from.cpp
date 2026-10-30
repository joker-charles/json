// cf_reflection_flat_union_from.cpp — negative compile test: the from_json
// direction runs the same structural validation as to_json.
// EXPECT-DIAG: does not support unions
#include <nlohmann/reflection.hpp>

union [[ = nlohmann::json_serializable {}]] bad
{
    int integer;
    float floating;
};

int main()
{
    const nlohmann::json j = nlohmann::json::object();
    (void)j.get<bad>(); // must not compile
}
