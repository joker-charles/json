// probe_type_alignment.cpp — verify the refl2 struct codec handles the full
// upstream type surface when such types appear as STRUCT MEMBERS. This is the
// "align with upstream" guard: every type nlohmann has a dedicated / generic
// overload for must serialize identically whether it is a top-level value or a
// member of a reflected struct.
//
// The four historical gaps fixed here (see the is_library_dedicated_array
// routing in reflection_to_json.hpp):
//   * binary_t      -> was a number array, must be {"bytes":[...],"subtype":...}
//   * C array       -> was a priority-0 static_assert, must route to the C-array overload
//   * forward_list  -> from_json had no matching branch (needs front_inserter)
//   * valarray      -> from_json aborted (default size 0; needs resize)
// These now route through the adl branch (the library's own overloads).
//
// Build: g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
//            -o /tmp/pta tests/static-reflection/probe_type_alignment.cpp && /tmp/pta
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <forward_list>
#include <string>
#include <tuple>
#include <utility>
#include <valarray>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

struct SBinary   { json::binary_t bin; };
struct SCArray   { int a[3]; };
struct SFList    { std::forward_list<int> l; };
struct SValarray { std::valarray<double> v; };
struct SPair     { std::pair<int, std::string> p; };
struct STuple    { std::tuple<int, double> t; };
struct SVBool    { std::vector<bool> v; };
struct SBareBin  { std::vector<std::uint8_t> v; };   // upstream: number array, not binary
struct SPath     { std::filesystem::path p; };

static int g_fail = 0;
static void check(const char* what, bool ok)
{
    std::printf("  %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) { ++g_fail; }
}

int main()
{
    std::printf("probe_type_alignment — upstream type surface as struct members\n\n");

    // binary_t: the dedicated binary form (not a number array)
    {
        SBinary s; s.bin = json::binary({1, 2, 255});
        json j; refl2::codec<false>::to_json(j, s);
        check("binary_t -> {\"bytes\":[...],\"subtype\":...}",
              j["bin"].is_binary() && j["bin"] == json::binary({1, 2, 255}));
        SBinary s2; refl2::codec<false>::from_json(j, s2);
        check("binary_t round-trip", s2.bin == s.bin);
    }

    // C array: routes to the C-array overload (was a static_assert)
    {
        SCArray s{{1, 2, 3}};
        json j; refl2::codec<false>::to_json(j, s);
        check("C array -> [1,2,3]", j["a"] == json::array({1, 2, 3}));
        SCArray s2{}; refl2::codec<false>::from_json(j, s2);
        check("C array round-trip", s2.a[0] == 1 && s2.a[1] == 2 && s2.a[2] == 3);
    }

    // forward_list: front_inserter path (was a compile error on from_json)
    {
        SFList s; s.l = {1, 2, 3};
        json j; refl2::codec<false>::to_json(j, s);
        check("forward_list -> [1,2,3]", j["l"] == json::array({1, 2, 3}));
        SFList s2; refl2::codec<false>::from_json(j, s2);
        check("forward_list round-trip", s2.l == s.l);
    }

    // valarray: resize path (was an abort on from_json)
    {
        SValarray s; s.v = {1.5, 2.5, 3.5};
        json j; refl2::codec<false>::to_json(j, s);
        check("valarray -> [1.5,2.5,3.5]", j["v"] == json::array({1.5, 2.5, 3.5}));
        SValarray s2; refl2::codec<false>::from_json(j, s2);
        check("valarray round-trip",
              s2.v.size() == 3 && s2.v[0] == 1.5 && s2.v[1] == 2.5 && s2.v[2] == 3.5);
    }

    // pair / tuple: array form (upstream behavior), not a reflected object
    {
        SPair s; s.p = {1, "x"};
        json j; refl2::codec<false>::to_json(j, s);
        check("pair -> [1,\"x\"]", j["p"] == json::array({1, "x"}));
        STuple st; st.t = {1, 2.5};
        json jt; refl2::codec<false>::to_json(jt, st);
        check("tuple -> [1,2.5]", jt["t"] == json::array({1, 2.5}));
    }

    // vector<bool> and bare vector<uint8_t>: number arrays (upstream behavior)
    {
        SVBool s; s.v = {true, false, true};
        json j; refl2::codec<false>::to_json(j, s);
        check("vector<bool> -> [true,false,true]", j["v"] == json::array({true, false, true}));
        SBareBin sb; sb.v = {1, 2, 255};
        json jb; refl2::codec<false>::to_json(jb, sb);
        check("bare vector<uint8_t> -> number array (not binary)",
              jb["v"] == json::array({1, 2, 255}));
    }

    // filesystem::path: string form
    {
        SPath s; s.p = "/tmp/x";
        json j; refl2::codec<false>::to_json(j, s);
        check("path -> string", j["p"] == "/tmp/x");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PROBE PASS" : "PROBE FAIL",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
