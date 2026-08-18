// type_optin_diff.cpp — the per-type opt-in gate (UPSTREAM_INTEGRATION_PLAN.md
// §2.2) under a differential lens: with JSON_USE_REFLECTION defined, a struct
// carrying the [[=refl2::json_serializable{}]] type-level annotation
// serializes through the reflection catch-all, and its output is
// byte-identical to the NLOHMANN_DEFINE_TYPE_* macro path (zero drift).
//
// Covers (g++-16, -std=c++26 -freflection):
//   1. annotated struct <-> NLOHMANN_DEFINE_TYPE_INTRUSIVE: to_json dump
//      byte-identical, from_json round-trip equal;
//   2. json_name annotation <-> _WITH_NAMES macro family (key override);
//   3. json_default annotation <-> _WITH_DEFAULT macro family (missing-key
//      fallback on from_json);
//   4. nested positions (struct member, vector element, map value,
//      optional member);
//   5. M6 annotated enum <-> NLOHMANN_JSON_SERIALIZE_ENUM string mapping;
//   6. M7 std::variant whitelist: a library type needs NO annotation.
//
// The unannotated / macro-off negatives live in compile_fail/
// (cf_unannotated_struct.cpp, cf_reflection_off.cpp).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Iinclude -o type_optin_diff type_optin_diff.cpp && ./type_optin_diff
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using json = nlohmann::json;

namespace refl_side
{
struct [[ = refl2::json_serializable {}]] Plain
{
    int a;
    std::string b;
};

struct [[ = refl2::json_serializable {}]] WithNames
{
    int x [[ = refl2::json_name{"display"}]];
    int y;
};

struct [[ = refl2::json_serializable {}]] WithDefault
{
    int a;
    int d [[ = refl2::json_default{}]];
};

struct [[ = refl2::json_serializable {}]] Outer
{
    int id;
    Plain p;
    std::vector<Plain> ps;
    std::map<std::string, Plain> m;
    std::optional<int> opt;
};
} // namespace refl_side

namespace macro_side
{
struct Plain
{
    int a;
    std::string b;
};
struct WithNames
{
    int x;
    int y;
};
struct WithDefault
{
    int a;
    int d;
};
struct Outer
{
    int id;
    Plain p;
    std::vector<Plain> ps;
    std::map<std::string, Plain> m;
    std::optional<int> opt;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Plain, a, b)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_NAMES(WithNames, "display", x, "y", y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WithDefault, a, d)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Outer, id, p, ps, m, opt)
} // namespace macro_side

enum class Color
{
    red [[ = refl2::json_name{"RED"}]],
    green [[ = refl2::json_name{"GREEN"}]]
};

enum class MacroColor
{
    red,
    green
};
NLOHMANN_JSON_SERIALIZE_ENUM(MacroColor,
{
    {MacroColor::red, "RED"},
    {MacroColor::green, "GREEN"}
})

static int failures = 0;

static void check(const char* what, bool ok)
{
    if (!ok)
    {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

int main()
{
    // 1. plain struct, byte-identical to the macro path
    const refl_side::Plain rp{1, "hello"};
    const macro_side::Plain mp{1, "hello"};
    const json jr = rp;
    const json jm = mp;
    check("plain to_json identical", jr.dump() == jm.dump() && jr.dump() == "{\"a\":1,\"b\":\"hello\"}");
    check("plain from_json round-trip", jr.get<refl_side::Plain>().a == 1 && jr.get<refl_side::Plain>().b == "hello");

    // 2. json_name <-> _WITH_NAMES
    const refl_side::WithNames rn{7, 8};
    const macro_side::WithNames mn{7, 8};
    check("json_name to_json identical", json(rn).dump() == json(mn).dump() && json(rn).dump() == "{\"display\":7,\"y\":8}");

    // 3. json_default <-> _WITH_DEFAULT (missing-key fallback)
    json miss = json::object({{"a", 5}});
    const auto rd = miss.get<refl_side::WithDefault>();
    const auto md = miss.get<macro_side::WithDefault>();
    check("json_default fallback equal", rd.a == md.a && rd.d == md.d);

    // 4. nested positions
    const refl_side::Outer ro{3, {1, "x"}, {{2, "y"}, {3, "z"}}, {{"k", {4, "w"}}}, 9};
    const macro_side::Outer mo{3, {1, "x"}, {{2, "y"}, {3, "z"}}, {{"k", {4, "w"}}}, 9};
    check("nested to_json identical", json(ro).dump() == json(mo).dump());
    check("nested from_json round-trip", json(ro).get<refl_side::Outer>().opt == 9);

    // 5. M6 enum string mapping (annotation vs macro)
    check("enum to_json identical", json(Color::green).dump() == json(MacroColor::green).dump() && json(Color::green).dump() == "\"GREEN\"");
    check("enum from_json round-trip", json("RED").get<Color>() == Color::red);

    // 6. M7 variant whitelist — library type, no annotation required
    std::variant<int, std::string> v{42};
    const json jv = v;
    check("variant whitelist to_json", jv.dump() == "{\"index\":0,\"value\":42}");
    check("variant whitelist from_json", jv.get<decltype(v)>() == v);

    if (failures == 0)
    {
        std::puts("type_optin_diff OK: annotated structs byte-identical to macro path; M6 enum; M7 variant");
        return 0;
    }
    std::printf("type_optin_diff: %d FAILURES\n", failures);
    return 1;
}
