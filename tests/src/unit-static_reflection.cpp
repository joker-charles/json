//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (supporting code)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Static reflection opt-in extension tests (feature/static-reflection):
// the reflection catch-all is a behavior change, so it compiles only when the
// user defines JSON_USE_REFLECTION (the include chain and
// reflection_to_json.hpp both enforce this), and only classes carrying the
// type-level [[=refl2::json_serializable{}]] annotation participate. This file
// deliberately references JSON_HAS_CPP_26 so the harness registers a C++26
// build with -freflection; it is C++26-only and excluded from the default
// standards in tests/CMakeLists.txt.

#define JSON_USE_REFLECTION 1

#include "doctest_compatibility.h"

#include <nlohmann/json.hpp>
using nlohmann::json;

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// --- annotation-driven reflection side ---
namespace reflect
{
struct [[=refl2::json_serializable{}]] Point
{
    double x;
    double y;
};
struct [[=refl2::json_serializable{}]] WithNames
{
    int x [[=refl2::json_name{"display"}]];
    int y;
};
struct [[=refl2::json_serializable{}]] WithDefault
{
    int a;
    int d [[=refl2::json_default{}]];
};
struct [[=refl2::json_serializable{}]] Partial
{
    int keep;
    int skip [[=refl2::json_ignore{}]];
};
struct [[=refl2::json_serializable{}]] Outer
{
    int id;
    Point p;
    std::vector<Point> ps;
    std::map<std::string, Point> m;
    std::optional<int> opt;
};
} // namespace reflect

// --- macro baseline side (the zero-drift reference) ---
namespace macro
{
struct Point
{
    double x;
    double y;
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_NAMES(WithNames, "display", x, "y", y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WithDefault, a, d)
} // namespace macro

// M6: annotated enum <-> NLOHMANN_JSON_SERIALIZE_ENUM baseline
enum class Color
{
    red [[=refl2::json_name{"RED"}]],
    green [[=refl2::json_name{"GREEN"}]]
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

TEST_CASE("static reflection opt-in: annotated structs serialize")
{
    SECTION("plain annotated struct")
    {
        const json j = reflect::Point {1.0, 2.0};
        CHECK(j.dump() == "{\"x\":1.0,\"y\":2.0}");
        const auto p = j.get<reflect::Point>();
        CHECK(p.x == doctest::Approx(1.0));
        CHECK(p.y == doctest::Approx(2.0));
    }

    SECTION("json_name overrides the member key")
    {
        const json j = reflect::WithNames {7, 8};
        CHECK(j.dump() == "{\"display\":7,\"y\":8}");
    }

    SECTION("json_ignore excludes the member")
    {
        const json j = reflect::Partial {1, 2};
        CHECK(j.dump() == "{\"keep\":1}");
    }

    SECTION("json_default falls back on missing keys")
    {
        const json j = json::object({{"a", 5}});
        const auto d = j.get<reflect::WithDefault>();
        CHECK(d.a == 5);
        CHECK(d.d == 0);
    }

    SECTION("nested positions: member, vector, map, optional")
    {
        const json j = reflect::Outer {3, {1.0, 2.0}, {{3.0, 4.0}}, {{"k", {5.0, 6.0}}}, 9};
        CHECK(j.dump() == "{\"id\":3,\"m\":{\"k\":{\"x\":5.0,\"y\":6.0}},\"opt\":9,\"p\":{\"x\":1.0,\"y\":2.0},\"ps\":[{\"x\":3.0,\"y\":4.0}]}");
        const auto o = j.get<reflect::Outer>();
        CHECK(o.id == 3);
        CHECK(o.opt == 9);
        CHECK(o.ps.size() == 1);
        CHECK(o.ps[0].x == doctest::Approx(3.0));
    }
}

TEST_CASE("static reflection opt-in: byte-identical to the NLOHMANN_DEFINE_TYPE_* path")
{
    SECTION("plain struct")
    {
        CHECK(json(reflect::Point {1.0, 2.0}).dump() == json(macro::Point {1.0, 2.0}).dump());
    }

    SECTION("json_name <-> _WITH_NAMES")
    {
        CHECK(json(reflect::WithNames {7, 8}).dump() == json(macro::WithNames {7, 8}).dump());
    }

    SECTION("json_default <-> _WITH_DEFAULT missing-key fallback")
    {
        const json miss = json::object({{"a", 5}});
        const auto rd = miss.get<reflect::WithDefault>();
        const auto md = miss.get<macro::WithDefault>();
        CHECK(rd.a == md.a);
        CHECK(rd.d == md.d);
    }
}

TEST_CASE("static reflection enum string mapping (M6)")
{
    SECTION("annotated enum maps to strings like NLOHMANN_JSON_SERIALIZE_ENUM")
    {
        CHECK(json(Color::green).dump() == json(MacroColor::green).dump());
        CHECK(json(Color::green).dump() == "\"GREEN\"");
        CHECK(json("RED").get<Color>() == Color::red);
    }

    SECTION("unannotated enum keeps the integer path")
    {
        enum class PlainE
        {
            a,
            b
        };
        CHECK(json(PlainE::b).dump() == "1");
        CHECK(json(1).get<PlainE>() == PlainE::b);
    }
}

TEST_CASE("static reflection std::variant whitelist (M7)")
{
    using V = std::variant<int, std::string>;

    SECTION("to_json emits the oneof wire format")
    {
        const json j = V {42};
        CHECK(j.dump() == "{\"index\":0,\"value\":42}");
    }

    SECTION("round-trip")
    {
        const json j = V {std::string("hi")};
        CHECK(j.get<V>() == V {std::string("hi")});
    }

    SECTION("out-of-range index throws type_error")
    {
        const json j = {{"index", 5}, {"value", 1}};
        CHECK_THROWS_WITH_AS(static_cast<void>(j.get<V>()),
                             "[json.exception.type_error.302] cannot parse variant: index 5 out of range (0..1)",
                             json::type_error);
    }

    SECTION("missing index throws out_of_range")
    {
        const json j = {{"value", 1}};
        CHECK_THROWS_WITH_AS(static_cast<void>(j.get<V>()),
                             "[json.exception.out_of_range.403] key 'index' not found",
                             json::out_of_range);
    }
}
