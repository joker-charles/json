// optin_namespace_wide.cpp — the namespace-wide opt-in from Anketell's first
// proposal, with JSON_REFLECTION_NAMESPACE_UNCONSTRAINED.
//
// In this mode the plain functions drop the is_reflectable<T> constraint, so a
// namespace `using` alone opts in every class/union in that namespace. The
// fixture proves the capability and the precedence; the hard-error side of the
// caveat is proven by namespace_compile_fail/cf_wide_private_member.cpp.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o optin_namespace_wide \
//       tests/static-reflection/optin_namespace_wide.cpp
//   ./optin_namespace_wide

#define JSON_USE_REFLECTION
#define JSON_REFLECTION_NAMESPACE_UNCONSTRAINED

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <string>

using json = nlohmann::json;

namespace wide_flat
{
struct type
{
    int x{};
    std::string s{};
};

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace wide_flat

namespace wide_custom
{
struct type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace wide_custom

namespace wide_macro
{
struct type
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(type, x)

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace wide_macro

static int failures = 0;

static void check(const char* label, bool ok)
{
    std::printf("  %-58s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++failures;
    }
}

int main()
{
    std::printf("optin_namespace_wide — namespace-wide `using` (unconstrained)\n\n");

    std::printf("[1] using alone opts in a flat struct\n");
    const json j1 = wide_flat::type{1, "a"};
    check("wide using: dump", j1.dump() == R"({"s":"a","x":1})");
    check("wide using: round-trip", j1.get<wide_flat::type>().s == "a");

    std::printf("\n[2] a non-template custom ADL serializer still wins\n");
    const json j2 = wide_custom::type{1};
    check("wide using + custom: custom wins", j2.dump() == "999");
    check("wide using + custom: from_json", json(999).get<wide_custom::type>().x == 999);

    std::printf("\n[3] NLOHMANN_DEFINE_TYPE_* still wins over the generic\n");
    const json j3 = wide_macro::type{1, 2};
    check("wide using + macro: macro wins", j3.dump() == R"({"x":1})");

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
