// cf_reflection_flat_not_in_json.cpp — negative compile test: the standalone
// flat-struct extension is an explicit add-on. Including only json.hpp (even
// under -freflection) must not expose nlohmann::json_serializable; users must
// include <nlohmann/reflection.hpp>.
// EXPECT-DIAG: json_serializable
#include <nlohmann/json.hpp>

struct [[ = nlohmann::json_serializable {}]] annotated
{
    int value{};
};

int main() {}
