// reflection_namespace.cpp — runtime fixture for
// <nlohmann/reflection_namespace.hpp>.
//
// This fixture proves the three opt-in routes on the public-API-only core:
//
//   * annotation ([[=nlohmann::json_serializable{}]]) — unchanged;
//   * per-type `nlohmann::reflection::enable<T>` — one specialization, no
//     `using` required, works for a type you do not own;
//   * namespace `using` in the default (constrained) mode — does NOT opt a
//     type in by itself; it is only a visibility declaration for types that
//     are already enabled/annotated.
//
// It also locks the precedence difference that motivated the comparison:
// `enable<T>` selects the forwarding adl_serializer specialization and
// therefore bypasses a hand-written ADL to_json/from_json, while a namespace
// `using` without `enable` goes through ADL and lets the non-template custom
// serializer win.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o reflection_namespace \
//       tests/static-reflection/reflection_namespace.cpp && ./reflection_namespace
//
// Single-header layout:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Isingle_include -Iinclude -o reflection_namespace_single \
//       tests/static-reflection/reflection_namespace.cpp && ./reflection_namespace_single

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using json = nlohmann::json;

namespace ann
{
struct [[=nlohmann::json_serializable{}]] point
{
    double x{};
    double y{};
};
} // namespace ann

namespace en
{
struct point
{
    double x{};
    double y{};
};

struct outer
{
    int id{};
    point p{};
    std::vector<point> ps{};
    std::optional<int> opt{};
};

struct empty
{
};
} // namespace en

namespace third_party
{
struct widget
{
    int id{};
};

// A type that already has its own serializer. enable<custom> below selects the
// forwarding adl_serializer specialization, which bypasses these functions.
struct custom
{
    int id{};
};

void to_json(json& j, const custom& c)
{
    j = c.id;
}

void from_json(const json& j, custom& c)
{
    c.id = j.get<int>();
}
} // namespace third_party

namespace using_only
{
struct plain
{
    int x{};
};

// Only a namespace-level using: in the default constrained mode this is not an
// opt-in, so `plain` must stay non-serializable.
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;

// A custom serializer still wins here because no enable<T> selects the
// forwarding adl_serializer specialization.
struct custom
{
    int id{};
};

void to_json(json& j, const custom& c)
{
    j = c.id;
}

void from_json(const json& j, custom& c)
{
    c.id = j.get<int>();
}
} // namespace using_only

namespace outer_scope
{
using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;

namespace inner
{
// A using-declaration in a non-inline enclosing namespace is not visible to
// ADL for a type in the nested namespace.
struct nested
{
    int x{};
};
} // namespace inner
} // namespace outer_scope

namespace both
{
struct point
{
    int x{};
};

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace both

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<en::point> = true;
template<> inline constexpr bool enable<en::outer> = true;
template<> inline constexpr bool enable<en::empty> = true;
template<> inline constexpr bool enable<third_party::widget> = true;
template<> inline constexpr bool enable<third_party::custom> = true;
template<> inline constexpr bool enable<both::point> = true;
} // namespace nlohmann::reflection

static_assert(nlohmann::reflection::is_reflectable_v<en::point>);
static_assert(nlohmann::reflection::is_reflectable_v<third_party::widget>);
static_assert(nlohmann::reflection::is_reflectable_v<ann::point>);
static_assert(!nlohmann::reflection::is_reflectable_v<using_only::plain>);
static_assert(!nlohmann::reflection::is_reflectable_v<outer_scope::inner::nested>);

static_assert(std::is_constructible_v<json, ann::point>);
static_assert(std::is_constructible_v<json, en::point>);
static_assert(std::is_constructible_v<json, third_party::widget>);
static_assert(std::is_constructible_v<json, both::point>);
static_assert(!std::is_constructible_v<json, using_only::plain>);
static_assert(!std::is_constructible_v<json, outer_scope::inner::nested>);

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
    std::printf("reflection_namespace — opt-in routes on the public-API core\n\n");

    std::printf("[1] annotation route still works alongside the namespace header\n");
    const json ja = ann::point{1.5, 2.5};
    check("annotation: dump", ja.dump() == R"({"x":1.5,"y":2.5})");
    check("annotation: round-trip", ja.get<ann::point>().y == 2.5);

    std::printf("\n[2] per-type enable<T> with no `using`\n");
    const json je = en::point{3.0, 4.0};
    check("enable: dump", je.dump() == R"({"x":3.0,"y":4.0})");
    const auto pe = je.get<en::point>();
    check("enable: round-trip", pe.x == 3.0 && pe.y == 4.0);

    std::printf("\n[3] a third-party type you do not own\n");
    const json jw = third_party::widget{7};
    check("third-party enable: dump", jw.dump() == R"({"id":7})");
    check("third-party enable: round-trip", jw.get<third_party::widget>().id == 7);

    std::printf("\n[4] enable<T> bypasses a hand-written ADL serializer\n");
    const json jc = third_party::custom{9};
    check("enable + custom ADL: reflection wins", jc.dump() == R"({"id":9})");
    check("enable + custom ADL: round-trip", jc.get<third_party::custom>().id == 9);

    std::printf("\n[5] namespace `using` alone is not an opt-in (constrained mode)\n");
    check("using-only type stays non-constructible", !std::is_constructible_v<json, using_only::plain>);
    check("nested namespace not covered by outer using", !std::is_constructible_v<json, outer_scope::inner::nested>);

    std::printf("\n[6] namespace `using` + custom ADL: the custom serializer wins\n");
    const json juc = using_only::custom{11};
    check("using-only + custom ADL: custom wins", juc.dump() == "11");
    check("using-only + custom ADL: round-trip", juc.get<using_only::custom>().id == 11);

    std::printf("\n[7] enable<T> + namespace `using` together\n");
    const json jb = both::point{13};
    check("enable + using: dump", jb.dump() == R"({"x":13})");
    check("enable + using: round-trip", jb.get<both::point>().x == 13);

    std::printf("\n[8] nested enable-only members and containers\n");
    const en::outer o{3, {1.0, 2.0}, {{3.0, 4.0}}, 9};
    const json jo = o;
    check("nested enable: dump",
          jo.dump() == R"({"id":3,"opt":9,"p":{"x":1.0,"y":2.0},"ps":[{"x":3.0,"y":4.0}]})");
    const auto ro = jo.get<en::outer>();
    check("nested enable: round-trip", ro.id == 3 && ro.opt == 9 && ro.ps.size() == 1);

    std::printf("\n[9] empty struct and ordered_json\n");
    const json jempty = en::empty{};
    check("empty: object", jempty.is_object() && jempty.empty());
    check("empty: round-trip", (jempty.get<en::empty>(), true));

    nlohmann::ordered_json oj = en::point{5.0, 6.0};
    check("ordered_json: to_json", oj.at("x").get<double>() == 5.0);
    check("ordered_json: from_json", oj.get<en::point>().y == 6.0);

    std::printf("\n[10] object-access semantics match the annotation route\n");
    bool missing_threw = false;
    try
    {
        (void)json{{"x", 1.0}}.get<en::point>();
    }
    catch (const json::out_of_range& e)
    {
        missing_threw = (e.id == 403);
    }
    check("missing key throws out_of_range 403", missing_threw);

    bool type_threw = false;
    try
    {
        (void)json(5).get<en::point>();
    }
    catch (const json::type_error& e)
    {
        type_threw = (e.id == 304);
    }
    check("non-object throws type_error 304", type_threw);

    const auto extra = json{{"x", 1.0}, {"y", 2.0}, {"extra", 3}}.get<en::point>();
    check("extra keys are ignored", extra.x == 1.0 && extra.y == 2.0);

    std::printf("\n[11] direct qualified calls to the plain functions\n");
    json jdirect;
    nlohmann::reflection::to_json(jdirect, en::point{8.0, 9.0});
    check("direct to_json", jdirect.dump() == R"({"x":8.0,"y":9.0})");
    en::point pdirect{};
    nlohmann::reflection::from_json(jdirect, pdirect);
    check("direct from_json", pdirect.x == 8.0 && pdirect.y == 9.0);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
