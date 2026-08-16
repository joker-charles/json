// probe_enum_reflection.cpp — M6 probe: reflection-driven enum string mapping
// ([[=refl2::json_name{"..."}]] on enumerators) replaces
// NLOHMANN_JSON_SERIALIZE_ENUM; unannotated enums keep the integer path.
//
// Build lines:
//   reflection: g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
//                 -o /tmp/per_r probe_enum_reflection.cpp && /tmp/per_r
//   baseline:   g++-16 -std=c++26 -O1 -Iinclude \
//                 -o /tmp/per_b probe_enum_reflection.cpp && /tmp/per_b
//   zero drift: diff <(grep '^COMMON' <(./per_b)) <(grep '^COMMON' <(./per_r)) == empty
//
// COMMON — unannotated enums (integer path) and macro-mapped enums, identical
// in both builds. REFL — annotation-mapped enums (reflection only).
#include <cstdio>
#include <string>
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

// unannotated enum: integer path in BOTH builds (zero drift)
enum class PlainE
{
    a = 1,
    b = 2
};

// macro-mapped enum (the thing M6 replaces)
namespace macro_side
{
enum class ColorM
{
    red = 1,
    green = 2,
    big = 3
};
NLOHMANN_JSON_SERIALIZE_ENUM(ColorM,
{
    {ColorM::red, "red"},
    {ColorM::green, "GREEN"},
    {ColorM::big, "big"}
})
} // namespace macro_side

static void run_common()
{
    common_check("unannotated enum -> integer", (json(PlainE::b).dump() == "2"));
    common_check("unannotated enum from_json", (json(2).get<PlainE>() == PlainE::b));
    common_check("macro enum -> string", (json(macro_side::ColorM::green).dump() == "\"GREEN\""));
    common_check("macro enum from_json", (json("GREEN").get<macro_side::ColorM>() == macro_side::ColorM::green));
}

#if defined(__cpp_impl_reflection)
// annotation-mapped enum (the M6 replacement)
enum class ColorR
{
    red = 1,
    green [[ = refl2::json_name{"GREEN"}]] = 2,
    big = 3 // unannotated -> enumerator name "big"
};

enum class SignedR
{
    neg = -1,
    pos [[ = refl2::json_name{"POS"}]] = 5
};

enum class UnsignedR : unsigned
{
    lo = 1u,
    hi [[ = refl2::json_name{"HIGH"}]] = 300u
};

// annotated enum nested in a reflected struct (recursion through the M5 codec)
struct WithEnum
{
    int id{};
    ColorR color;
    std::vector<ColorR> palette;
};

static void run_refl()
{
    // to_json: annotation override, identifier fallback, integer-free path
    refl_check("annotated enum -> json_name string",
               (json(ColorR::green).dump() == "\"GREEN\""));
    refl_check("unannotated enumerator -> identifier",
               (json(ColorR::big).dump() == "\"big\""));
    refl_check("plain enumerator -> identifier",
               (json(ColorR::red).dump() == "\"red\""));
    refl_check("signed negative enumerator",
               (json(SignedR::neg).dump() == "\"neg\""));
    refl_check("unsigned enum",
               (json(UnsignedR::hi).dump() == "\"HIGH\""));

    // macro <-> annotation differential: same mapping, byte-identical output
    refl_check("macro == reflection (green)",
               (json(macro_side::ColorM::green).dump() == json(ColorR::green).dump()));
    refl_check("macro == reflection (red)",
               (json(macro_side::ColorM::red).dump() == json(ColorR::red).dump()));
    refl_check("macro == reflection (big)",
               (json(macro_side::ColorM::big).dump() == json(ColorR::big).dump()));

    // from_json: string -> enum (annotation + identifier)
    refl_check("string GREEN -> green", (json("GREEN").get<ColorR>() == ColorR::green));
    refl_check("string big -> big", (json("big").get<ColorR>() == ColorR::big));
    refl_check("string POS -> pos", (json("POS").get<SignedR>() == SignedR::pos));

    // from_json: integer fallback (documented divergence from the macro)
    refl_check("int 2 -> green (integer fallback)", (json(2).get<ColorR>() == ColorR::green));
    refl_check("int -1 -> neg", (json(-1).get<SignedR>() == SignedR::neg));

    // unknown string -> type_error 302
    bool threw = false;
    try
    {
        (void)json("NOPE").get<ColorR>();
    }
    catch (const json::type_error& e)
    {
        threw = (e.id == 302);
    }
    refl_check("unknown string throws 302", threw);

    // containers + nested struct member
    refl_check("vector<annotated enum>",
               (json(std::vector<ColorR> {ColorR::red, ColorR::green}).dump() == R"(["red","GREEN"])"));
    auto v2 = json(std::vector<ColorR> {ColorR::red, ColorR::green}).get<std::vector<ColorR>>();
    refl_check("vector round-trip", (v2.size() == 2 && v2[1] == ColorR::green));

    WithEnum we{7, ColorR::green, {ColorR::red, ColorR::big}};
    json jw = we;
    refl_check("enum member inside reflected struct",
               (jw.dump() == R"({"color":"GREEN","id":7,"palette":["red","big"]})"));
    auto we2 = jw.get<WithEnum>();
    refl_check("nested enum round-trip", (we2.color == ColorR::green && we2.palette[1] == ColorR::big));
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
