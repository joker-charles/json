// probe_complex_diff.cpp — macro vs reflection ZERO-DRIFT on COMPLEX structs.
//
// Layer 1 of the "complex struct comparison" (the macro-coverable surface
// only): deeply-nested structs, multi-level inheritance, and mixed ordered
// containers. For each shape we define a MACRO version (NLOHMANN_DEFINE_TYPE_*
// — whitelist, so the macro arguments must list every member in declaration
// order) and a REFLECTION version (zero boilerplate), build IDENTICAL values,
// then diff to_json byte-for-byte and diff the from_json round-trip.
//
// NOTE on why two types per shape: a struct that carries a macro's to_json is
// handled by the macro (customization wins over reflection), so the same type
// cannot exercise both paths. Two structurally-identical types let the two
// paths produce comparable output.
//
// Build: g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
//            -o /tmp/pcd tests/static-reflection/probe_complex_diff.cpp && /tmp/pcd
// opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#define JSON_USE_REFLECTION 1
#include <array>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// (1) deeply-nested recursive tree
// ---------------------------------------------------------------------------
struct MacroLeaf
{
    int value;
    std::string label;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroLeaf, value, label)
struct MacroNode
{
    int id;
    std::string name;
    MacroLeaf leaf;
    std::vector<MacroLeaf> leaves;
    std::vector<MacroNode> children;
    std::map<std::string, MacroLeaf> by_name;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroNode, id, name, leaf, leaves, children, by_name)

struct [[ = refl2::json_serializable {}]] ReflLeaf { int value; std::string label; };
struct [[ = refl2::json_serializable {}]] ReflNode
{
    int id;
    std::string name;
    ReflLeaf leaf;
    std::vector<ReflLeaf> leaves;
    std::vector<ReflNode> children;
    std::map<std::string, ReflLeaf> by_name;
};

// ---------------------------------------------------------------------------
// (2) multi-level inheritance
// ---------------------------------------------------------------------------
struct MacroBase
{
    int a;
    std::string b;
};
struct MacroMid : MacroBase
{
    double c;
    std::vector<int> d;
};
struct MacroDerived : MacroMid
{
    bool e;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroDerived, a, b, c, d, e)

struct ReflBase
{
    int a;
    std::string b;
};
struct ReflMid : ReflBase
{
    double c;
    std::vector<int> d;
};
struct [[ = refl2::json_serializable {}]] ReflDerived :
ReflMid { bool e; };

// ---------------------------------------------------------------------------
// (3) mixed ordered containers
// ---------------------------------------------------------------------------
struct MacroMix
{
    std::vector<int> vi;
    std::list<double> ld;
    std::deque<std::string> ds;
    std::set<int> si;
    std::map<std::string, int> msi;
    std::array<int, 3> arr;
    std::pair<int, std::string> pr;
    std::tuple<int, double, std::string> tu;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroMix, vi, ld, ds, si, msi, arr, pr, tu)

struct [[ = refl2::json_serializable {}]] ReflMix
{
    std::vector<int> vi;
    std::list<double> ld;
    std::deque<std::string> ds;
    std::set<int> si;
    std::map<std::string, int> msi;
    std::array<int, 3> arr;
    std::pair<int, std::string> pr;
    std::tuple<int, double, std::string> tu;
};

static int g_fail = 0;

static void check_diff(const char* what, const json& jm, const json& jr)
{
    if (jm.dump() == jr.dump())
    {
        std::printf("  [PASS] %-44s\n", what);
    }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s\n    macro: %s\n    refl : %s\n",
                    what, jm.dump().c_str(), jr.dump().c_str());
    }
}

int main()
{
    std::printf("probe_complex_diff — macro vs reflection on complex structs\n\n");

    // ---- (1) deeply-nested recursive tree --------------------------------
    {
        MacroNode mn{1, "root",
            {10, "l0"},
            {{11, "l1"}, {12, "l2"}},
            {   {2, "c1", {20, "c1l"}, {}, {}, {}},
                {
                    3, "c2", {30, "c2l"}, {{31, "x"}}, {},
                    {{"k", {32, "v"}}}
                }
            },
            {{"a", {13, "la"}}, {"b", {14, "lb"}}}};
        ReflNode rn{1, "root",
            {10, "l0"},
            {{11, "l1"}, {12, "l2"}},
            {   {2, "c1", {20, "c1l"}, {}, {}, {}},
                {
                    3, "c2", {30, "c2l"}, {{31, "x"}}, {},
                    {{"k", {32, "v"}}}
                }
            },
            {{"a", {13, "la"}}, {"b", {14, "lb"}}}};
        check_diff("deep tree to_json", json(mn), json(rn));
        check_diff("deep tree round-trip", json(json(mn).get<MacroNode>()),
                   json(json(rn).get<ReflNode>()));
    }

    // ---- (2) multi-level inheritance -------------------------------------
    {
        // MacroDerived = { Mid{ Base{a,b}, c, d }, e }
        MacroDerived md{ {{1, "base"}, 2.5, {3, 4, 5}}, true };
        ReflDerived  rd{ {{1, "base"}, 2.5, {3, 4, 5}}, true };
        check_diff("inheritance to_json", json(md), json(rd));
        check_diff("inheritance round-trip", json(json(md).get<MacroDerived>()),
                   json(json(rd).get<ReflDerived>()));
    }

    // ---- (3) mixed ordered containers ------------------------------------
    {
        MacroMix mm{{1, 2, 3}, {1.5, 2.5}, {"a", "b"}, {3, 1, 2},
            {{"x", 10}, {"y", 20}}, {{7, 8, 9}}, {1, "p"}, {1, 2.5, "t"}};
        ReflMix rm{{1, 2, 3}, {1.5, 2.5}, {"a", "b"}, {3, 1, 2},
            {{"x", 10}, {"y", 20}}, {{7, 8, 9}}, {1, "p"}, {1, 2.5, "t"}};
        check_diff("mixed containers to_json", json(mm), json(rm));
        check_diff("mixed containers round-trip", json(json(mm).get<MacroMix>()),
                   json(json(rm).get<ReflMix>()));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PROBE PASS" : "PROBE FAIL",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
