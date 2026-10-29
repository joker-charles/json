// reflection_flat_struct.cpp — runtime fixture for <nlohmann/reflection.hpp>.
//
// This fixture opts in with JSON_USE_REFLECTION and includes ONLY
// <nlohmann/reflection.hpp> (no separate json.hpp include) and proves the
// extension's contract:
//
//   * [[=nlohmann::json_serializable{}]] is a per-type opt-in; unannotated
//     structs keep the main library's behavior (not constructible into json);
//   * flat structs round-trip through the public basic_json API;
//   * nested members use the normal path (ADL to_json/from_json, library
//     containers, or another annotated type);
//   * empty structs map to {}, missing keys throw 403, extra keys are ignored;
//   * the same serializer works with nlohmann::ordered_json.
//
// Build (split headers):
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
//       -o /tmp/reflection_flat_struct \
//       tests/static-reflection/reflection_flat_struct.cpp
//   /tmp/reflection_flat_struct
//
// Build (amalgamated library + split add-on):
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o /tmp/reflection_flat_struct_single \
//       tests/static-reflection/reflection_flat_struct.cpp
//   /tmp/reflection_flat_struct_single

#define JSON_USE_REFLECTION

#include <nlohmann/reflection.hpp>

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using json = nlohmann::json;

struct [[ = nlohmann::json_serializable {}]] point
{
    double x{};
    double y{};
};

struct plain
{
    int value{};
};

struct [[ = nlohmann::json_serializable {}]] empty
{
};

namespace adl
{
struct widget
{
    int id{};
};

void to_json(json& j, const widget& w)
{
    j = w.id;
}

void from_json(const json& j, widget& w)
{
    w.id = j.get<int>();
}
} // namespace adl

namespace nested
{
struct [[ = nlohmann::json_serializable {}]] inner
{
    int a{};
};

struct [[ = nlohmann::json_serializable {}]] outer
{
    inner i;
    adl::widget w;
    std::string name;
    std::vector<int> values;
};
} // namespace nested

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
    std::printf("reflection_flat_struct — standalone per-type extension\n\n");

    // The annotation is the only opt-in: the unannotated type must not become
    // constructible from json just because the header is included.
    static_assert(std::is_constructible_v<json, point>);
    static_assert(!std::is_constructible_v<json, plain>);

    std::printf("[1] flat struct round-trip\n");
    const point p{1.5, 2.5};
    json j = p;
    check("to_json builds an object", j.is_object());
    check("to_json value x", j.at("x").get<double>() == 1.5);
    check("to_json value y", j.at("y").get<double>() == 2.5);
    check("to_json dump", j.dump() == R"({"x":1.5,"y":2.5})");

    const auto p2 = j.get<point>();
    check("from_json value x", p2.x == 1.5);
    check("from_json value y", p2.y == 2.5);

    json assigned;
    assigned = p;
    check("assignment uses the same serializer", assigned == j);

    std::printf("\n[2] empty struct\n");
    json je = empty{};
    check("empty: object", je.is_object());
    check("empty: no members", je.empty());
    check("empty: dump", je.dump() == "{}");
    (void)je.get<empty>();

    std::printf("\n[3] nested members: annotated + ADL + library container\n");
    const nested::outer o{nested::inner{7}, adl::widget{9}, "name", {1, 2, 3}};
    json jo = o;
    check("nested: inner annotated member", jo.at("i").at("a").get<int>() == 7);
    check("nested: ADL member", jo.at("w").get<int>() == 9);
    check("nested: string member", jo.at("name").get<std::string>() == "name");
    check("nested: vector member", jo.at("values") == json({1, 2, 3}));

    const auto o2 = jo.get<nested::outer>();
    check("nested: round-trip inner", o2.i.a == 7);
    check("nested: round-trip ADL", o2.w.id == 9);
    check("nested: round-trip string", o2.name == "name");
    check("nested: round-trip vector", o2.values == std::vector<int>({1, 2, 3}));

    std::printf("\n[4] object access semantics\n");
    bool missing_threw = false;
    try
    {
        (void)json{{"x", 1.0}}.get<point>();
    }
    catch (const json::out_of_range& e)
    {
        missing_threw = (e.id == 403);
    }
    check("missing key throws out_of_range 403", missing_threw);

    const json extra = {{"x", 1.0}, {"y", 2.0}, {"z", 3}};
    const auto pe = extra.get<point>();
    check("extra keys are ignored", pe.x == 1.0 && pe.y == 2.0);

    std::printf("\n[5] generic BasicJsonType\n");
    nlohmann::ordered_json oj = p;
    check("ordered_json: to_json", oj.at("x").get<double>() == 1.5);
    const auto op = oj.get<point>();
    check("ordered_json: from_json", op.x == 1.5 && op.y == 2.5);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
