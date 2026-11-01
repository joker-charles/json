// semantics_matrix.cpp — Q3 contract: missing / unknown / type-error behavior.
//
// The reflection core uses `j.at(key).get<Member>()` for every member, so the
// matrix is shared by C1 annotation, C2 enable<T> and C3 namespace using. It
// is compared with the plain NLOHMANN_DEFINE_TYPE_* path (parity baseline) and
// with _WITH_DEFAULT (the feature reflection v1 does not have).
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o semantics_matrix tests/static-reflection/semantics_matrix.cpp
//   ./semantics_matrix

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace refl
{
struct nested
{
    int n{};
};

struct all
{
    int i{};
    std::string s{};
    std::vector<int> v{};
    std::optional<int> o{};
    nested n{};
};
} // namespace refl

namespace macro_plain
{
struct nested
{
    int n{};
};

struct all
{
    int i{};
    std::string s{};
    std::vector<int> v{};
    std::optional<int> o{};
    nested n{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(nested, n)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(all, i, s, v, o, n)
} // namespace macro_plain

namespace macro_default
{
struct nested
{
    int n{};
};

struct all
{
    int i{};
    std::string s{};
    std::vector<int> v{};
    std::optional<int> o{};
    nested n{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(nested, n)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(all, i, s, v, o, n)
} // namespace macro_default

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<refl::nested> = true;
template<> inline constexpr bool enable<refl::all> = true;
} // namespace nlohmann::reflection

static int failures = 0;

static void check(const char* label, bool ok)
{
    std::printf("  %-62s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++failures;
    }
}

template<typename F>
static void expect_id(const char* label, int expected, F&& f)
{
    try
    {
        f();
        std::printf("  %-62s FAIL (no exception)\n", label);
        ++failures;
    }
    catch (const json::exception& e)
    {
        if (e.id == expected)
        {
            std::printf("  %-62s PASS (id %d)\n", label, e.id);
        }
        else
        {
            std::printf("  %-62s FAIL (id %d, expected %d: %s)\n",
                        label, e.id, expected, e.what());
            ++failures;
        }
    }
}

static json full()
{
    return json{
        {"i", 1},
        {"s", "x"},
        {"v", json::array({1, 2})},
        {"o", 3},
        {"n", {{"n", 4}}}};
}

int main()
{
    std::printf("semantics_matrix — missing / unknown / type errors\n\n");

    std::printf("[1] reflection baseline\n");
    expect_id("missing scalar", 403, [] { (void)json{{"s", "x"}, {"v", json::array()}, {"o", 1}, {"n", {{"n", 1}}}}.get<refl::all>(); });
    expect_id("missing optional", 403, [] { (void)json{{"i", 1}, {"s", "x"}, {"v", json::array()}, {"n", {{"n", 1}}}}.get<refl::all>(); });
    expect_id("missing nested", 403, [] { (void)json{{"i", 1}, {"s", "x"}, {"v", json::array()}, {"o", 1}, {"n", json::object()}}.get<refl::all>(); });
    expect_id("empty object", 403, [] { (void)json::object().get<refl::all>(); });
    expect_id("null for int", 302, [] { auto j = full(); j["i"] = nullptr; (void)j.get<refl::all>(); });
    expect_id("wrong int type", 302, [] { auto j = full(); j["i"] = "x"; (void)j.get<refl::all>(); });
    expect_id("wrong vector type", 302, [] { auto j = full(); j["v"] = 5; (void)j.get<refl::all>(); });
    expect_id("array input", 304, [] { (void)json::array().get<refl::all>(); });
    expect_id("null input", 304, [] { (void)json(nullptr).get<refl::all>(); });

    try
    {
        const auto v = full().get<refl::all>();
        check("optional with value", v.o == 3);
    }
    catch (...)
    {
        check("optional with value", false);
    }

    try
    {
        auto j = full();
        j["o"] = nullptr;
        const auto v = j.get<refl::all>();
        check("optional null -> nullopt", !v.o.has_value());
    }
    catch (...)
    {
        check("optional null -> nullopt", false);
    }

    try
    {
        auto j = full();
        j["extra"] = 99;
        const auto v = j.get<refl::all>();
        check("extra key ignored", v.i == 1 && v.s == "x");
    }
    catch (...)
    {
        check("extra key ignored", false);
    }

    std::printf("\n[2] macro parity (plain)\n");
    expect_id("plain macro: missing optional", 403, [] { (void)json{{"i", 1}, {"s", "x"}, {"v", json::array()}, {"n", {{"n", 1}}}}.get<macro_plain::all>(); });
    expect_id("plain macro: null for int", 302, [] { auto j = full(); j["i"] = nullptr; (void)j.get<macro_plain::all>(); });
    expect_id("plain macro: array input", 304, [] { (void)json::array().get<macro_plain::all>(); });

    std::printf("\n[3] WITH_DEFAULT gap (reflection v1 has no default)\n");
    try
    {
        const auto v = json{{"i", 1}, {"s", "x"}, {"v", json::array()}, {"n", {{"n", 1}}}}.get<macro_default::all>();
        check("WITH_DEFAULT: missing optional -> nullopt", !v.o.has_value());
    }
    catch (...)
    {
        check("WITH_DEFAULT: missing optional -> nullopt", false);
    }
    expect_id("reflection: missing optional still 403", 403, [] { (void)json{{"i", 1}, {"s", "x"}, {"v", json::array()}, {"n", {{"n", 1}}}}.get<refl::all>(); });

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
