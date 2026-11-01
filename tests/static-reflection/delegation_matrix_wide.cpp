// delegation_matrix_wide.cpp — the same "own serializer delegates" use case in
// the unconstrained namespace mode.
//
// With JSON_REFLECTION_NAMESPACE_UNCONSTRAINED the plain functions accept any
// complete class, and a non-template custom to_json/from_json in the type's
// namespace beats the reflection template in ADL. This is the only current
// route where the type's own serializer runs and can delegate to the plain
// reflection functions.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o delegation_matrix_wide \
//       tests/static-reflection/delegation_matrix_wide.cpp
//   ./delegation_matrix_wide

#define JSON_USE_REFLECTION
#define JSON_REFLECTION_NAMESPACE_UNCONSTRAINED

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>

using json = nlohmann::json;

static bool to_called = false;
static bool from_called = false;

namespace delegated
{
struct type
{
    int x{};
};

void to_json(json& j, const type& value)
{
    to_called = true;
    nlohmann::reflection::to_json(j, value);
    j["via_custom"] = true;
}

void from_json(const json& j, type& value)
{
    from_called = true;
    nlohmann::reflection::from_json(j, value);
}

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace delegated

static int failures = 0;

static void check(const char* label, bool ok)
{
    std::printf("  %-62s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++failures;
    }
}

int main()
{
    std::printf("delegation_matrix_wide — unconstrained namespace mode\n\n");

    const json j = delegated::type{4};
    check("C3 unconstrained: own to_json ran", to_called);
    check("C3 unconstrained: wrapper delegated",
          j.dump() == R"({"via_custom":true,"x":4})");

    const auto value = j.get<delegated::type>();
    check("C3 unconstrained: own from_json ran", from_called);
    check("C3 unconstrained: from_json delegated", value.x == 4);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
