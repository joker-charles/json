// cf_variant_json.cpp — negative compile test: a std::variant containing a
// `json` alternative is a documented M7 boundary (the `json` alternative sits
// inside the library's own namespace tree, so the catch-all's in_json_namespace
// exclusion removes it from the eligible set). Compilation MUST fail at the
// converting constructor.
//
// NOTE: this asserts a CURRENT limitation, not a permanent contract. If M7
// later adds json-alternative support, update/remove this case.
// EXPECT-DIAG: non-scalar type
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>
#include <variant>

using json = nlohmann::json;
using V = std::variant<int, json>;

int main()
{
    V v = 1;
    json out = v; // must not compile (in_json_namespace exclusion)
}
