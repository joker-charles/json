// delegation_matrix.cpp — the maintainer's "use it from a type's own
// to_json/from_json" use case, in the default (constrained) mode.
//
// A type's own serializer is a non-template free function in its namespace (or
// an intrusive hidden friend). The wrapper below delegates to
// nlohmann::reflection::to_json/from_json and sets a marker, so the fixture can
// tell whether the wrapper actually ran.
//
// Expected: C1 annotation, C2 enable<T>, and C3 using + enable all select an
// adl_serializer specialization that bypasses the wrapper. The marker stays
// false; the output is produced directly by the shared core. The unconstrained
// C3 route (delegation_matrix_wide.cpp) is the one that lets the wrapper run.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o delegation_matrix tests/static-reflection/delegation_matrix.cpp
//   ./delegation_matrix

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>

using json = nlohmann::json;

static bool ann_to_called = false;
static bool ann_from_called = false;
static bool en_to_called = false;
static bool en_from_called = false;
static bool both_to_called = false;
static bool both_from_called = false;

namespace ann_case
{
struct [[=nlohmann::json_serializable{}]] type
{
    int x{};
};
} // namespace ann_case

namespace en_case
{
struct type
{
    int x{};
};
} // namespace en_case

namespace both_case
{
struct type
{
    int x{};
};
} // namespace both_case

// enable<T> must be visible before the wrapper bodies call the constrained
// reflection functions.
namespace nlohmann::reflection
{
template<> inline constexpr bool enable<en_case::type> = true;
template<> inline constexpr bool enable<both_case::type> = true;
} // namespace nlohmann::reflection

namespace ann_case
{
void to_json(json& j, const type& value)
{
    ann_to_called = true;
    nlohmann::reflection::to_json(j, value);
}

void from_json(const json& j, type& value)
{
    ann_from_called = true;
    nlohmann::reflection::from_json(j, value);
}
} // namespace ann_case

namespace en_case
{
void to_json(json& j, const type& value)
{
    en_to_called = true;
    nlohmann::reflection::to_json(j, value);
}

void from_json(const json& j, type& value)
{
    en_from_called = true;
    nlohmann::reflection::from_json(j, value);
}
} // namespace en_case

namespace both_case
{
void to_json(json& j, const type& value)
{
    both_to_called = true;
    nlohmann::reflection::to_json(j, value);
}

void from_json(const json& j, type& value)
{
    both_from_called = true;
    nlohmann::reflection::from_json(j, value);
}

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace both_case

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
    std::printf("delegation_matrix — constrained mode\n\n");

    std::printf("[1] C1 annotation + own serializer\n");
    const json j1 = ann_case::type{1};
    check("C1: output from reflection", j1.dump() == R"({"x":1})");
    check("C1: own to_json bypassed", !ann_to_called);
    const auto r1 = j1.get<ann_case::type>();
    check("C1: own from_json bypassed", !ann_from_called && r1.x == 1);

    std::printf("\n[2] C2 enable<T> + own serializer\n");
    const json j2 = en_case::type{2};
    check("C2: output from reflection", j2.dump() == R"({"x":2})");
    check("C2: own to_json bypassed", !en_to_called);
    const auto r2 = j2.get<en_case::type>();
    check("C2: own from_json bypassed", !en_from_called && r2.x == 2);

    std::printf("\n[3] C3 using + enable<T> + own serializer\n");
    const json j3 = both_case::type{3};
    check("C3 constrained: output from reflection", j3.dump() == R"({"x":3})");
    check("C3 constrained: own to_json bypassed", !both_to_called);
    const auto r3 = j3.get<both_case::type>();
    check("C3 constrained: own from_json bypassed", !both_from_called && r3.x == 3);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
