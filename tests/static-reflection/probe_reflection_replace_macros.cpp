// probe_reflection_replace_macros.cpp — M5 probe: the reflection-driven
// generic to_json/from_json (library catch-all, see M5_REFLECTION_TO_JSON.md)
// replaces the NLOHMANN_DEFINE_TYPE_* macro family for reflectable structs.
//
// Build lines:
//   reflection: g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
//                 -o /tmp/prm_r probe_reflection_replace_macros.cpp && /tmp/prm_r
//   baseline:   g++-16 -std=c++26 -O1 -Iinclude \
//                 -o /tmp/prm_b probe_reflection_replace_macros.cpp && /tmp/prm_b
//   zero drift: diff <(grep '^COMMON' /tmp/out_b) <(grep '^COMMON' /tmp/out_r) == empty
// (the reflection TU also prints REFL| lines; both runs must exit 0)
//
// Sections:
//   COMMON — behavior that must be identical with and without the reflection
//            gate: existing library types (scalars, strings, containers,
//            pair/tuple, enum) and CUSTOMIZED types (macro, free to_json,
//            adl_serializer specialization) must serialize exactly as before.
//   REFL   — reflection-only cases (guarded by __cpp_impl_reflection): plain
//            structs, macro<->annotation differential (4 pairs), round-trip,
//            annotation negatives, private-member policy, nested
//            customization precedence.
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int g_fail = 0;

static void common_check(const char* what, bool ok)
{
    std::printf("COMMON| %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++g_fail;
    }
}

static void refl_check(const char* what, bool ok)
{
    std::printf("REFL|   %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++g_fail;
    }
}

// ---------------------------------------------------------------------------
// COMMON — library types and CUSTOMIZED types (identical in both builds)
// ---------------------------------------------------------------------------

// (a) existing library types
enum class Color { red = 1, green = 2 };

// (b) macro-defined type (NON_INTRUSIVE — free functions in the namespace)
struct MacroPlain
{
    int x{};
    std::string y;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroPlain, x, y)

// (c) hand-written free to_json/from_json (defined in the TYPE's namespace —
//     that is what makes them visible to ADL)
namespace mine
{
struct FreeCustom
{
    int hidden{};
};
void to_json(json& j, const FreeCustom& f)
{
    j = json::object({{"custom", f.hidden}});
}
void from_json(const json& j, FreeCustom& f)
{
    f.hidden = j.at("custom").get<int>();
}
} // namespace mine

// (d) adl_serializer<T, void> specialization (the documented shape)
struct AdlCustom
{
    int v{};
};
template<>
struct nlohmann::adl_serializer<AdlCustom, void>
{
    static void to_json(json& j, const AdlCustom& c)
    {
        j = json::object({{"adl", c.v}});
    }
    static void from_json(const json& j, AdlCustom& c)
    {
        c.v = j.at("adl").get<int>();
    }
};

// (e) a struct that is ALSO reflectable (public members) but customized —
//     customization must win over reflection in the reflection build
namespace mine
{
struct ReflectableButCustom
{
    int a{};
    int b{};
};
void to_json(json& j, const ReflectableButCustom& c)
{
    j = json::object({{"custom_a", c.a}, {"custom_b", c.b}});
}
} // namespace mine

static void run_common()
{
    // scalars / strings / containers / pair / tuple / enum — untouched paths
    common_check("scalar int", (json(42).dump() == "42"));
    common_check("string", (json("hi").dump() == "\"hi\""));
    common_check("vector<int>", (json(std::vector<int> {1, 2}).dump() == "[1,2]"));
    common_check("map<string,int>", (json(std::map<std::string, int> {{"a", 1}}).dump() == R"({"a":1})"));
    common_check("pair", (json(std::pair<int, int> {1, 2}).dump() == "[1,2]"));
    common_check("tuple", (json(std::tuple<int, int> {1, 2}).dump() == "[1,2]"));
    common_check("enum", (json(Color::green).dump() == "2"));

    // customized types — same behavior in both builds (zero drift)
    json jm = MacroPlain{1, "s"};
    common_check("macro type", (jm.dump() == R"({"x":1,"y":"s"})"));

    json jf = mine::FreeCustom{9};
    common_check("free to_json custom", (jf.dump() == R"({"custom":9})"));
    mine::FreeCustom f2;
    jf.get_to(f2);
    common_check("free from_json custom", (f2.hidden == 9));

    json ja = AdlCustom{5};
    common_check("adl_serializer specialization", (ja.dump() == R"({"adl":5})"));
    AdlCustom a2;
    ja.get_to(a2);
    common_check("adl_serializer from_json", (a2.v == 5));

    json jc = mine::ReflectableButCustom{1, 2};
    common_check("customized wins over reflection", (jc.dump() == R"({"custom_a":1,"custom_b":2})"));
}

#if defined(__cpp_impl_reflection)
// ---------------------------------------------------------------------------
// REFL — reflection-only cases
// ---------------------------------------------------------------------------

// macro <-> annotation differential pairs (same members, same keys -> the
// JSON must be byte-identical)
namespace macro_side
{
struct Plain
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Plain, x, y)

struct WithNames
{
    int a{};
    int b{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_NAMES(WithNames, "display", a, "other", b)

struct WithDefault
{
    int a{};
    int b{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WithDefault, a, b)

struct Partial
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Partial, x) // only x listed
} // namespace macro_side

namespace refl_side
{
struct Plain
{
    int x{};
    int y{};
};

struct WithNames
{
    [[ = refl2::json_name{"display"}]] int a;
    [[ = refl2::json_name{"other"}]] int b;
};

struct WithDefault
{
    int a;
    [[ = refl2::json_default{}]] int b;
};

struct Partial
{
    int x{};
    [[ = refl2::json_ignore{}]] int y;
};
} // namespace refl_side

// nested customization: a reflected struct containing a customized member
struct NestedCustom
{
    int id{};
    mine::FreeCustom fc;
    std::vector<mine::FreeCustom> fcs;
};

// private members: codec<false> (the catch-all default) must NOT serialize
// them; codec<true> (explicit opt-in) does. (A class with private data
// members is not an aggregate — hence the constructor.)
struct WithPrivate
{
    WithPrivate(int p, int s) : pub(p), secret(s) {}
    int pub{};
  private:
    int secret{};
};

// optional member + bit-field member
struct OptionalBits
{
    std::optional<int> o;
    int bf : 3;
    int tail{};
};

static void run_refl()
{
    // --- plain struct via the library entry points ---
    json jp = refl_side::Plain{1, 2};
    refl_check("plain struct -> json", (jp.dump() == R"({"x":1,"y":2})"));
    auto p2 = jp.get<refl_side::Plain>();
    refl_check("json -> plain struct (get)", (p2.x == 1 && p2.y == 2));
    json jo;
    jo["point"] = refl_side::Plain{3, 4};
    refl_side::Plain p3;
    jo.at("point").get_to(p3);
    refl_check("j[key] = struct + get_to", (p3.x == 3 && p3.y == 4));
    refl_check("is_object", (jo.at("point").is_object()));

    // --- containers of structs ---
    json jv = std::vector<refl_side::Plain> {{1, 2}, {3, 4}};
    refl_check("vector<struct>", (jv.dump() == R"([{"x":1,"y":2},{"x":3,"y":4}])"));
    json jm = std::map<std::string, refl_side::Plain> {{"a", {1, 2}}};
    refl_check("map<string,struct>", (jm.dump() == R"({"a":{"x":1,"y":2}})"));
    auto vm = jv.get<std::vector<refl_side::Plain>>();
    refl_check("vector<struct> round-trip", (vm.size() == 2 && vm[1].y == 4));

    // --- macro <-> annotation differential: byte-identical output ---
    refl_check("plain: macro == reflection",
               (json(macro_side::Plain{1, 2}).dump() == json(refl_side::Plain{1, 2}).dump()));
    refl_check("WITH_NAMES == json_name",
               (json(macro_side::WithNames{1, 2}).dump() == json(refl_side::WithNames{1, 2}).dump()));
    refl_check("  (json_name keys)", (json(refl_side::WithNames{1, 2}).dump() == R"({"display":1,"other":2})"));
    refl_check("WITH_DEFAULT == json_default (to_json)",
               (json(macro_side::WithDefault{1, 2}).dump() == json(refl_side::WithDefault{1, 2}).dump()));
    refl_check("partial == json_ignore",
               (json(macro_side::Partial{1, 2}).dump() == json(refl_side::Partial{1, 2}).dump()));
    refl_check("  (json_ignore key absent)", (!json(refl_side::Partial{1, 2}).contains("y")));

    // --- from_json defaults: missing key falls back to T{}.member ---
    json jmiss = json::object({{"a", 7}});
    refl_side::WithDefault d1 = jmiss.get<refl_side::WithDefault>();
    refl_check("json_default missing key -> T{} member", (d1.a == 7 && d1.b == 0));
    macro_side::WithDefault d2 = jmiss.get<macro_side::WithDefault>();
    refl_check("WITH_DEFAULT missing key -> T{} member", (d2.a == 7 && d2.b == 0));

    // --- optional / bit-field members ---
    json job = OptionalBits{5, 3, 9};
    refl_check("optional + bit-field to_json", (job.dump() == R"({"bf":3,"o":5,"tail":9})"));
    auto ob2 = job.get<OptionalBits>();
    refl_check("optional + bit-field round-trip", (ob2.o.has_value() && *ob2.o == 5 && ob2.bf == 3 && ob2.tail == 9));
    json jobn = OptionalBits{std::nullopt, 1, 2};
    refl_check("optional null", (jobn.dump() == R"({"bf":1,"o":null,"tail":2})"));

    // --- private-member policy ---
    WithPrivate wp{1, 2};
    json jwp = wp; // catch-all = codec<false> = unprivileged
    refl_check("private member NOT serialized (unprivileged)",
               (jwp.dump() == R"({"pub":1})"));
    json jwpu;
    refl2::codec<true>::to_json(jwpu, wp); // explicit opt-in = unchecked
    refl_check("private member serialized via codec<true>",
               (jwpu.dump() == R"({"pub":1,"secret":2})"));

    // --- nested customization precedence (pitfall fix) ---
    json jn = NestedCustom{7, {8}, {{9}}};
    refl_check("nested customized member uses user to_json",
               (jn.dump() == R"({"fc":{"custom":8},"fcs":[{"custom":9}],"id":7})"));
    auto nc2 = jn.get<NestedCustom>();
    refl_check("nested customized member from_json",
               (nc2.id == 7 && nc2.fc.hidden == 8 && nc2.fcs[0].hidden == 9));

    // --- adl_serializer<T, void> specialization on a reflectable struct ---
    json jc = AdlCustom{6};
    refl_check("adl specialization wins (top level)", (jc.dump() == R"({"adl":6})"));
    json jcf;
    refl2::codec<false>::to_json(jcf, AdlCustom{7});
    refl_check("adl specialization wins (through codec)", (jcf.dump() == R"({"adl":7})"));
}
#endif // __cpp_impl_reflection

int main()
{
    std::printf("== common (baseline and reflection must match) ==\n");
    run_common();
#if defined(__cpp_impl_reflection)
    std::printf("== reflection-only ==\n");
    run_refl();
#endif
    std::printf(g_fail == 0 ? "\nALL CHECKS PASSED\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
