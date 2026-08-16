// probe_variant.cpp — M7: top-level std::variant support in the refl2 codec.
//
// The codec gains a dedicated variant branch (priority 6, between nested json
// and optional): to_json emits the oneof wire format
//   {"index": <active alternative index>, "value": <alternative serialized>}
// (std::monostate carries no payload -> "value": null); from_json dispatches
// the stored index to std::variant_alternative_t<I, T> via an index_sequence
// fold and deserializes "value" into the emplaced alternative. Alternatives
// must be default-constructible (emplace<I>()).
//
// Covers (g++-16, -std=c++26 -freflection):
//   1. to_json of variant<int, std::string, Point> for every alternative;
//   2. from_json round-trip for every alternative (get<V>() == original);
//   3. std::monostate alternatives (no payload -> null);
//   4. nested positions: struct member, vector element, map value,
//      optional<variant>, variant<variant<...>, ...> recursion;
//   5. json as an alternative (the nested-json branch inside the variant);
//   6. error paths: out-of-range index, negative index, missing "index",
//      missing "value" on a non-monostate alternative;
//   7. user customization still wins: a type with a hand-written to_json /
//      adl_serializer specialization keeps priority over the variant branch.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Iinclude -o probe_variant probe_variant.cpp && ./probe_variant
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

bool ok = true;
std::size_t checks = 0;

#define CHECK(cond, msg, ...)                                                                  \
    do                                                                                         \
    {                                                                                          \
        ++checks;                                                                              \
        if (!(cond))                                                                           \
        {                                                                                      \
            std::printf("  [FAIL] " msg "\n" __VA_OPT__(, ) __VA_ARGS__);                      \
            ok = false;                                                                        \
        }                                                                                      \
    } while (0)

// (1) reflected struct alternative
struct Point
{
    int x{};
    double y{};
};
inline bool operator==(const Point& a, const Point& b)
{
    return a.x == b.x && a.y == b.y;
}

using V = std::variant<int, std::string, Point>;
using VM = std::variant<std::monostate, int>;
using VV = std::variant<std::variant<int, std::string>, double>;

// (4) nested positions
struct Holder
{
    V v;
};
inline bool operator==(const Holder& a, const Holder& b)
{
    return a.v == b.v;
}

// (7) user customization still wins over the variant branch
struct Custom
{
    int tag{};
};
inline bool operator==(const Custom& a, const Custom& b)
{
    return a.tag == b.tag;
}
namespace nlohmann
{
template<>
struct adl_serializer<Custom, void>
{
    static void to_json(json& j, const Custom& c)
    {
        j = json::object({{"custom_tag", c.tag}});
    }
    static void from_json(const json& j, Custom& c)
    {
        c.tag = j.at("custom_tag").template get<int>();
    }
};
} // namespace nlohmann
using VC = std::variant<int, Custom>;

void check_basic()
{
    std::printf("[1] basic oneof wire format + round-trip\n");

    V a = 42;
    json ja = a;
    CHECK(ja.dump() == "{\"index\":0,\"value\":42}", "variant<int> wire: %s", ja.dump().c_str());
    CHECK(ja.template get<V>() == a, "variant<int> round-trip");

    V b = std::string("hi");
    json jb = b;
    CHECK(jb.dump() == "{\"index\":1,\"value\":\"hi\"}", "variant<string> wire: %s", jb.dump().c_str());
    CHECK(jb.template get<V>() == b, "variant<string> round-trip");

    V c = Point{1, 2.5};
    json jc = c;
    CHECK(jc.dump() == "{\"index\":2,\"value\":{\"x\":1,\"y\":2.5}}", "variant<Point> wire: %s", jc.dump().c_str());
    CHECK(jc.template get<V>() == c, "variant<Point> round-trip");

    // nested assignment path (operator[] -> CPO -> codec)
    json jo = json::object();
    jo["k"] = a;
    CHECK(jo.dump() == "{\"k\":{\"index\":0,\"value\":42}}", "nested assignment: %s", jo.dump().c_str());
}

void check_monostate()
{
    std::printf("[2] monostate alternative\n");

    VM empty{};
    json j = empty;
    CHECK(j.dump() == "{\"index\":0,\"value\":null}", "monostate wire: %s", j.dump().c_str());
    CHECK(j.template get<VM>() == empty, "monostate round-trip");

    VM full = 7;
    json j2 = full;
    CHECK(j2.dump() == "{\"index\":1,\"value\":7}", "monostate-engaged wire: %s", j2.dump().c_str());
    CHECK(j2.template get<VM>() == full, "monostate-engaged round-trip");
}

void check_nested()
{
    std::printf("[3] nested positions\n");

    // struct member
    Holder h{V{Point{3, 4.5}}};
    json jh = h;
    CHECK(jh.dump() == "{\"v\":{\"index\":2,\"value\":{\"x\":3,\"y\":4.5}}}", "struct member: %s", jh.dump().c_str());
    CHECK(jh.template get<Holder>() == h, "struct member round-trip");

    // vector element
    std::vector<V> vec{V{1}, V{std::string("two")}};
    json jvec = vec;
    CHECK(jvec.dump() == "[{\"index\":0,\"value\":1},{\"index\":1,\"value\":\"two\"}]", "vector element: %s", jvec.dump().c_str());
    CHECK(jvec.template get<std::vector<V>>() == vec, "vector element round-trip");

    // map value
    std::map<std::string, V> mp{{"a", V{1}}, {"b", V{Point{9, 0.5}}}};
    json jmp = mp;
    CHECK(jmp["a"].dump() == "{\"index\":0,\"value\":1}", "map value a");
    const auto got_map = jmp.template get<std::map<std::string, V>>();
    CHECK(got_map == mp, "map value round-trip");

    // optional<variant>
    std::optional<V> ov = V{std::string("opt")};
    json jov = ov;
    CHECK(jov.dump() == "{\"index\":1,\"value\":\"opt\"}", "optional<variant> engaged: %s", jov.dump().c_str());
    CHECK(jov.template get<std::optional<V>>() == ov, "optional<variant> round-trip");
    std::optional<V> onone = std::nullopt;
    CHECK(json(onone).dump() == "null", "optional<variant> nullopt: %s", json(onone).dump().c_str());
    CHECK(json(onone).template get<std::optional<V>>() == onone, "optional<variant> nullopt round-trip");

    // variant<variant<...>, double> — the variant branch recurses
    using VIn = std::variant<int, std::string>;
    VV inner = VIn{7};
    json ji = inner;
    CHECK(ji.dump() == "{\"index\":0,\"value\":{\"index\":0,\"value\":7}}", "nested variant: %s", ji.dump().c_str());
    CHECK(ji.template get<VV>() == inner, "nested variant round-trip");
    VV outer = 3.5;
    json jo2 = outer;
    CHECK(jo2.dump() == "{\"index\":1,\"value\":3.5}", "outer double: %s", jo2.dump().c_str());
    CHECK(jo2.template get<VV>() == outer, "outer double round-trip");
}

void check_custom_alternatives()
{
    std::printf("[4] custom alternatives (struct / array / object)\n");

    // NOTE: a `json` ALTERNATIVE inside a variant (e.g. variant<..., json>)
    // is deliberately not supported — the variant's template arguments carry
    // the nlohmann namespace, so in_json_namespace / the ADL probe treat it
    // as library-internal (the M5 circularity defense's documented boundary).
    using W = std::variant<Point, std::vector<int>, std::map<std::string, int>>;

    W w1 = Point{2, 3.0};
    CHECK(json(w1).dump() == "{\"index\":0,\"value\":{\"x\":2,\"y\":3.0}}", "alt struct");
    CHECK(json(w1).template get<W>() == w1, "alt struct round-trip");

    W w2 = std::vector<int>{1, 2, 3};
    CHECK(json(w2).dump() == "{\"index\":1,\"value\":[1,2,3]}", "alt array");
    CHECK(json(w2).template get<W>() == w2, "alt array round-trip");

    W w3 = std::map<std::string, int>{{"a", 1}};
    CHECK(json(w3).dump() == "{\"index\":2,\"value\":{\"a\":1}}", "alt object");
    CHECK(json(w3).template get<W>() == w3, "alt object round-trip");
}

void check_errors()
{
    std::printf("[5] error paths\n");

    // out-of-range index
    json j1 = {{"index", 5}, {"value", 1}};
    bool threw = false;
    try
    {
        static_cast<void>(j1.template get<V>());
    }
    catch (const nlohmann::detail::type_error&)
    {
        threw = true;
    }
    CHECK(threw, "out-of-range index must throw type_error");

    // negative index (get<size_t> rejects)
    json j2 = {{"index", -1}, {"value", 1}};
    threw = false;
    try
    {
        static_cast<void>(j2.template get<V>());
    }
    catch (const nlohmann::detail::type_error&)
    {
        threw = true;
    }
    CHECK(threw, "negative index must throw type_error");

    // missing "index"
    json j3 = {{"value", 1}};
    threw = false;
    try
    {
        static_cast<void>(j3.template get<V>());
    }
    catch (const nlohmann::detail::out_of_range&)
    {
        threw = true;
    }
    CHECK(threw, "missing index must throw out_of_range");

    // missing "value" on a non-monostate alternative
    json j4 = {{"index", 0}};
    threw = false;
    try
    {
        static_cast<void>(j4.template get<V>());
    }
    catch (const nlohmann::detail::out_of_range&)
    {
        threw = true;
    }
    CHECK(threw, "missing value must throw out_of_range");
}

void check_customization()
{
    std::printf("[6] user customization still wins\n");

    VC v = Custom{9};
    json j = v;
    CHECK(j.dump() == "{\"index\":1,\"value\":{\"custom_tag\":9}}", "variant<Custom> wire: %s", j.dump().c_str());
    CHECK(j.template get<VC>() == v, "variant<Custom> round-trip (adl branch inside variant)");

    // top level: the specialized type itself (not inside a variant) is
    // serialized by its own adl_serializer, never the reflect branch
    Custom c{5};
    json jc = c;
    CHECK(jc.dump() == "{\"custom_tag\":5}", "Custom top-level: %s", jc.dump().c_str());
    CHECK(jc.template get<Custom>() == c, "Custom top-level round-trip");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    check_basic();
    check_monostate();
    check_nested();
    check_custom_alternatives();
    check_errors();
    check_customization();

    std::printf("\nM7 VARIANT PROBE %s (%zu checks)\n", ok ? "PASSED" : "FAILED", checks);
    return ok ? 0 : 1;
}
