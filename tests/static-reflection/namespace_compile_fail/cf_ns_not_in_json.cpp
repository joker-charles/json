// cf_ns_not_in_json.cpp — the namespace add-on is explicit: including only
// json.hpp must not expose nlohmann::reflection.
// EXPECT-DIAG: reflection
#include <nlohmann/json.hpp>

namespace dm
{
struct type
{
    int x{};
};
using nlohmann::reflection::to_json;
} // namespace dm

int main() {}
