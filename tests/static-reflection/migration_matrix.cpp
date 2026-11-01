// migration_matrix.cpp — Q4 evidence: what the macro family has that
// reflection v1 does not.
//
// The question is "complement or replacement?". This fixture demonstrates the
// concrete gaps that decide it:
//
//   * WITH_NAMES: macro emits renamed keys; reflection v1 emits identifiers;
//   * WITH_DEFAULT: macro fills missing members from a default object;
//     reflection v1 throws 403;
//   * ONLY_SERIALIZE: macro has no from_json; reflection always provides both.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o migration_matrix tests/static-reflection/migration_matrix.cpp
//   ./migration_matrix

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <string>

using json = nlohmann::json;

namespace macro_names
{
struct type
{
    int i{};
    std::string s{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_NAMES(type, "display", i, "why", s)
} // namespace macro_names

namespace refl_names
{
struct type
{
    int i{};
    std::string s{};
};
} // namespace refl_names

namespace macro_default
{
struct type
{
    int i{7};
    std::string s{"def"};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(type, i, s)
} // namespace macro_default

namespace refl_default
{
struct type
{
    int i{7};
    std::string s{"def"};
};
} // namespace refl_default

namespace macro_only
{
struct type
{
    int i{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(type, i)
} // namespace macro_only

namespace refl_only
{
struct type
{
    int i{};
};
} // namespace refl_only

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<refl_names::type> = true;
template<> inline constexpr bool enable<refl_default::type> = true;
template<> inline constexpr bool enable<refl_only::type> = true;
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

int main()
{
    std::printf("migration_matrix — macro family vs reflection v1\n\n");

    std::printf("[1] WITH_NAMES\n");
    check("macro uses renamed keys",
          json(macro_names::type{1, "x"}).dump() == R"({"display":1,"why":"x"})");
    check("reflection uses identifiers",
          json(refl_names::type{1, "x"}).dump() == R"({"i":1,"s":"x"})");

    std::printf("\n[2] WITH_DEFAULT\n");
    try
    {
        const auto v = json::object().get<macro_default::type>();
        check("macro missing keys -> defaults", v.i == 7 && v.s == "def");
    }
    catch (...)
    {
        check("macro missing keys -> defaults", false);
    }
    try
    {
        (void)json::object().get<refl_default::type>();
        check("reflection missing keys -> 403", false);
    }
    catch (const json::exception& e)
    {
        check("reflection missing keys -> 403", e.id == 403);
    }

    std::printf("\n[3] ONLY_SERIALIZE\n");
    static_assert(!nlohmann::detail::has_from_json<json, macro_only::type>::value,
                  "macro ONLY_SERIALIZE has no from_json");
    static_assert(nlohmann::detail::has_from_json<json, refl_only::type>::value,
                  "reflection always provides from_json");
    check("macro only-serialize has no from_json", true);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
