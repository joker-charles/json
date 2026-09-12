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
// M7's top-level std::variant support is a second, separately-gated behavior
// change (it would otherwise break upstream's `!is_constructible<json,
// std::variant<...>>` invariant, unit-regression2.cpp:569). This file opts in
// explicitly; the gate-on upstream targets (tests/CMakeLists.txt) compile
// without it and pin the default invariant.
#define JSON_USE_REFLECTION_VARIANT 1

#include "doctest_compatibility.h"

#include <nlohmann/json.hpp>
using nlohmann::json;

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// --- annotation-driven reflection side ---
namespace reflect
{
struct [[ = refl2::json_serializable {}]] Point
{
    double x;
    double y;
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
struct [[ = refl2::json_serializable {}]] Partial
{
    int keep;
    int skip [[ = refl2::json_ignore{}]];
};
struct [[ = refl2::json_serializable {}]] Outer
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

    SECTION("json_default member that IS present is read, not defaulted")
    {
        // the other half of the json_default contract: only the missing-key
        // path was covered before, so a present value silently defaulting
        // would not have been caught
        const json j = json::object({{"a", 5}, {"d", 7}});
        const auto d = j.get<reflect::WithDefault>();
        CHECK(d.a == 5);
        CHECK(d.d == 7);
    }

    SECTION("json_default: a present null is not a fallback (both paths agree)")
    {
        // null is a PRESENT value and is not convertible to int, so neither
        // path defaults it -- the macro and the reflection path throw alike
        const json j = json::object({{"a", 5}, {"d", nullptr}});
        CHECK_THROWS_WITH_AS(static_cast<void>(j.get<reflect::WithDefault>()),
                             "[json.exception.type_error.302] type must be number, but is null",
                             json::type_error);
        CHECK_THROWS_AS(static_cast<void>(j.get<macro::WithDefault>()), json::type_error);
    }

    SECTION("json_default: whole-value null -- documented divergence")
    {
        // _WITH_DEFAULT treats a null ENTIRE value as "every member default";
        // refl2 has no such fallback and throws. features/reflection.md lists
        // this as a known divergence, but it had no test.
        const json j = nullptr;
        const auto md = j.get<macro::WithDefault>();
        CHECK(md.a == 0);
        CHECK(md.d == 0);
        CHECK_THROWS_WITH_AS(static_cast<void>(j.get<reflect::WithDefault>()),
                             "[json.exception.type_error.304] cannot use at() with null",
                             json::type_error);
    }

    SECTION("a null value resets an optional member")
    {
        // the reset branch (the non-null emplace branch was the only covered
        // one), so an optional member silently keeping its previous value on
        // a null input would not have been caught
        const json j = json::parse(R"({"id":1,"p":{"x":1.0,"y":2.0},"ps":[],"m":{},"opt":null})");
        const auto o = j.get<reflect::Outer>();
        CHECK(o.id == 1);
        CHECK(!o.opt.has_value());
        // round-trips back to null, not to a default value
        CHECK(json(o).at("opt").is_null());
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

    SECTION("annotated enum accepts integers as well as its mapped strings")
    {
        // documented divergence from NLOHMANN_JSON_SERIALIZE_ENUM (which only
        // accepts table entries): the annotated path falls back to the integer
        // branch for any non-string JSON
        CHECK(json(1).get<Color>() == Color::green);
        CHECK(json(0).get<Color>() == Color::red);
        // unsigned and negative integers both go through enum_get_arithmetic
        CHECK(json(0u).get<Color>() == Color::red);
        CHECK(static_cast<int>(json(-1).get<Color>()) == -1);
        // float is the third arithmetic branch
        CHECK(json(1.0).get<Color>() == Color::green);
    }

    SECTION("annotated enum rejects a non-numeric, non-string value")
    {
        // previously uncovered: the enum_get_arithmetic else-branch
        CHECK_THROWS_WITH_AS(static_cast<void>(json(nullptr).get<Color>()),
                             "[json.exception.type_error.302] type must be number, but is null",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::array().get<Color>()),
                             "[json.exception.type_error.302] type must be number, but is array",
                             json::type_error);
    }

    SECTION("annotated enum throws on an unknown string")
    {
        // documented divergence: the macro silently falls back to the first
        // table entry, the annotated path reports the bad key
        CHECK_THROWS_WITH_AS(static_cast<void>(json("PURPLE").get<Color>()),
                             "[json.exception.type_error.302] cannot parse enum string 'PURPLE'",
                             json::type_error);
    }

    SECTION("annotated enum round-trips through its mapping")
    {
        for (const auto c :
                {
                    Color::red, Color::green
                })
        {
            CHECK(json(c).get<Color>() == c);
        }
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

// ---------------------------------------------------------------------------
// Regression coverage for defects found in review (2026-08). Each case below
// failed (or failed to compile) before the corresponding fix.
// ---------------------------------------------------------------------------

// opaque enum: `enum class X : T;` with no definition. std::meta::
// enumerators_of on it is a HARD consteval error, so the M6 path must fall
// back to the integer path instead of breaking the whole TU
// (upstream unit-udt.cpp uses exactly this shape).
enum class opaque_id : std::uint64_t;

// plain unannotated enum with NO NLOHMANN_JSON_SERIALIZE_ENUM: the integer
// path must stay noexcept
namespace plain_no_macro
{
enum class b { x, y };
} // namespace plain_no_macro

namespace reflect
{
struct [[ = refl2::json_serializable {}]] WithArray
{
    std::array<int, 3> arr;
};
struct [[ = refl2::json_serializable {}]] WithMap
{
    std::map<int, int> m;
};
struct [[ = refl2::json_serializable {}]] WithVariant
{
    std::variant<int, std::string> v;
};
} // namespace reflect

TEST_CASE("static reflection review regressions")
{
    SECTION("opaque enum keeps the integer path (no meta::exception)")
    {
        const json j = static_cast<opaque_id>(42);
        CHECK(j.dump() == "42");
        CHECK(j.get<opaque_id>() == static_cast<opaque_id>(42));
        // every arithmetic branch of the integer fallback, for an enum whose
        // underlying type is unsigned long (the annotated path uses the same
        // helper)
        CHECK(json(42u).get<opaque_id>() == static_cast<opaque_id>(42));
        CHECK(json(42.0).get<opaque_id>() == static_cast<opaque_id>(42));
        // a NEGATIVE integer is the signed branch: nlohmann stores a positive
        // literal as unsigned, so only this reaches the number_integer case
        // (it wraps through the unsigned underlying type)
        CHECK(static_cast<std::uint64_t>(json(-1).get<opaque_id>())
              == std::numeric_limits<std::uint64_t>::max());
        CHECK_THROWS_AS(static_cast<void>(json(nullptr).get<opaque_id>()), json::type_error);
    }

    SECTION("enum to_json stays noexcept for unannotated enums")
    {
        // upstream unit-noexcept.cpp asserts exactly this for both the free
        // CPO call and the basic_json converting constructor (it uses a plain
        // enum with no NLOHMANN_JSON_SERIALIZE_ENUM, whose to_json is not
        // noexcept either)
        static_assert(noexcept(nlohmann::to_json(std::declval<json&>(), plain_no_macro::b::x)), "");
        static_assert(noexcept(json(plain_no_macro::b::x)), "");
        static_assert(!noexcept(nlohmann::to_json(std::declval<json&>(), Color::red)),
                      "the annotated (allocating) path is not noexcept");
    }

    SECTION("std::array size mismatch throws instead of aborting")
    {
        // before the fix: std::abort() (SIGABRT) on a short array
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"arr":[1,2]})").get<reflect::WithArray>()),
                             "[json.exception.out_of_range.401] array index 2 is out of range",
                             json::out_of_range);
        // extra elements are ignored, like the library's std::array from_json
        const auto longer = json::parse(R"({"arr":[1,2,3,4]})").get<reflect::WithArray>();
        CHECK(longer.arr == std::array<int, 3> {1, 2, 3});
    }

    SECTION("non-numeric key for an arithmetic-keyed map throws instead of becoming 0")
    {
        // before the fix the key silently became 0 (data loss / key collision)
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"m":{"abc":1}})").get<reflect::WithMap>()),
                             "[json.exception.type_error.302] type must be number, but is \"abc\"",
                             json::type_error);
        const auto ok = json::parse(R"({"m":{"7":1}})").get<reflect::WithMap>();
        CHECK(ok.m.at(7) == 1);
    }

    SECTION("std::variant MEMBER still works without JSON_USE_REFLECTION_VARIANT")
    {
        // the per-type gate only affects TOP-LEVEL variants
        const json j = reflect::WithVariant {std::string("hi")};
        CHECK(j.dump() == R"({"v":{"index":1,"value":"hi"}})");
        CHECK(j.get<reflect::WithVariant>().v == std::variant<int, std::string> {std::string("hi")});
    }

    SECTION("a variant member with its FIRST alternative active round-trips")
    {
        // index 0 was never deserialized before: every variant fixture used
        // alternative 1, so the emplace<I=0> path went untested
        const json j = reflect::WithVariant {42};
        CHECK(j.dump() == R"({"v":{"index":0,"value":42}})");
        const auto back = j.get<reflect::WithVariant>();
        CHECK(back.v.index() == 0);
        CHECK(std::get<0>(back.v) == 42);
    }
}

// ---------------------------------------------------------------------------
// Public annotation inspection API (refl2::member_count / member / member_key /
// member_has_default / has_annotation / enum_*). This is what lets a user build
// annotation-driven tools outside the codec, so the contract worth pinning is
// that it reports EXACTLY what the codec serializes -- a tool and the wire
// format must not be able to disagree about which members exist or their keys.
// ---------------------------------------------------------------------------
TEST_CASE("static reflection: public annotation inspection API")
{
    SECTION("member_count excludes json_ignore members")
    {
        // Partial has keep + skip, and skip is [[=refl2::json_ignore{}]]
        static_assert(refl2::member_count<reflect::Partial> == 1, "");
        static_assert(refl2::member_count<reflect::Point> == 2, "");
        // Outer: id, p, ps, m, opt
        static_assert(refl2::member_count<reflect::Outer> == 5, "");
    }

    SECTION("member_key honours json_name and falls back to the identifier")
    {
        CHECK(std::string(refl2::member_key<reflect::WithNames, 0>.data()) == "display");
        CHECK(std::string(refl2::member_key<reflect::WithNames, 1>.data()) == "y");
        CHECK(std::string(refl2::member_key<reflect::Point, 0>.data()) == "x");
    }

    SECTION("member info is spliceable into a type")
    {
        using First = typename [: std::meta::type_of(refl2::member<reflect::Point, 0>) :];
        static_assert(std::is_same_v<First, double>, "");
        using Outer0 = typename [: std::meta::type_of(refl2::member<reflect::Outer, 0>) :];
        static_assert(std::is_same_v<Outer0, int>, "");
    }

    SECTION("member_has_default tracks json_default")
    {
        static_assert(refl2::member_has_default<reflect::WithDefault, 1>, "");
        static_assert(!refl2::member_has_default<reflect::WithDefault, 0>, "");
        static_assert(!refl2::member_has_default<reflect::Point, 0>, "");
    }

    SECTION("has_annotation works on members and on enumerators")
    {
        // json_ignore members are FILTERED OUT of refl2::member, so the
        // annotation is observable through the raw reflection query -- this is
        // exactly the documented difference between refl2::member (serialized
        // members only) and std::meta::nonstatic_data_members_of (source
        // members). Getting this backwards asserts the opposite of the design.
        static_assert(refl2::member_count<reflect::Partial> == 1, "");
        CHECK(std::string(refl2::member_key<reflect::Partial, 0>.data()) == "keep");
        static_assert(!refl2::has_annotation<refl2::json_ignore>(
                          std::meta::nonstatic_data_members_of(^^reflect::Partial,
                                  std::meta::access_context::unprivileged())[0]), "");
        static_assert(refl2::has_annotation<refl2::json_ignore>(
                          std::meta::nonstatic_data_members_of(^^reflect::Partial,
                                  std::meta::access_context::unprivileged())[1]), "");

        static_assert(refl2::has_annotation<refl2::json_name>(refl2::member<reflect::WithNames, 0>), "");
        static_assert(!refl2::has_annotation<refl2::json_name>(refl2::member<reflect::WithNames, 1>), "");
        static_assert(refl2::has_annotation<refl2::json_serializable>(^^reflect::Point), "");
        static_assert(refl2::has_annotation<refl2::json_serializable>(^^reflect::Outer), "");
        // negative control: an unannotated enum's enumerator carries no json_name
        static_assert(!refl2::has_annotation<refl2::json_name>(
                          std::meta::enumerators_of(^^MacroColor)[0]), "");
    }

    SECTION("enum queries")
    {
        static_assert(refl2::enum_is_enumerable<Color>(), "");
        static_assert(refl2::enum_has_annotations<Color>(), "");
        static_assert(refl2::enum_count<Color>() == 2, "");
        CHECK(std::string(refl2::enum_string<Color, 0>.data()) == "RED");
        CHECK(std::string(refl2::enum_string<Color, 1>.data()) == "GREEN");
        static_assert(refl2::enum_value<Color, 0> == 0, "");
        static_assert(refl2::enum_value<Color, 1> == 1, "");

        // an opaque enum declaration is NOT enumerable -- the documented
        // precondition for the other enum queries (enumerators_of on it is a
        // hard consteval error, not a fallback)
        static_assert(!refl2::enum_is_enumerable<opaque_id>(), "");
        // an unannotated enum is enumerable but carries no json_name
        static_assert(refl2::enum_is_enumerable<MacroColor>(), "");
        static_assert(!refl2::enum_has_annotations<MacroColor>(), "");
    }

    SECTION("the inspection API agrees with what the codec actually emits")
    {
        // The invariant that matters: for every annotated type, the keys the
        // API reports are exactly the keys in the serialized object. A tool
        // built on the API therefore cannot drift from the wire format.
        const auto keys_agree = [](const json & j, const std::vector<std::string>& reported)
        {
            std::vector<std::string> emitted;
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                emitted.push_back(it.key());
            }
            std::sort(emitted.begin(), emitted.end());
            auto want = reported;
            std::sort(want.begin(), want.end());
            return emitted == want;
        };

        const json jp = reflect::Partial {1, 2};
        REQUIRE(keys_agree(jp, {"keep"}));
        CHECK(!jp.contains("skip"));

        const json jn = reflect::WithNames {7, 8};
        REQUIRE(keys_agree(jn, {"display", "y"}));

        const json jo = reflect::Outer {1, {1.0, 2.0}, {}, {}, std::nullopt};
        REQUIRE(keys_agree(jo, {"id", "m", "opt", "p", "ps"}));
    }
}

// ---------------------------------------------------------------------------
// Dispatch precedence: a user-supplied to_json wins over the reflection branch.
//
// This is correct behaviour, and it is pinned here because getting it wrong is
// silent: refl2::detail::to_adl_branch_eligible_v becomes true as soon as the
// type has ANY to_json (including one generated by NLOHMANN_DEFINE_TYPE_*),
// and the codec's priority_tag<4> ADL branch outranks its priority_tag<1>
// reflection branch -- so the reflection codec is never instantiated for that
// type. A benchmark that gave its "reflection" structs the intrusive macro
// therefore measured the macro path twice and reported them as identical
// (docs/static-reflection/SCALING.md §3). The static_asserts below are the
// guard: if the precedence ever flips, this test fails loudly instead of
// quietly changing what a benchmark means.
// ---------------------------------------------------------------------------
namespace dispatch
{
// reflectable AND carrying a macro-provided to_json -> the ADL branch wins
struct [[ = refl2::json_serializable {}]] WithMacro
{
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WithMacro, v)
    int v;
};
// reflectable with no customization -> the reflection branch
struct [[ = refl2::json_serializable {}]] Plain
{
    int v;
};
} // namespace dispatch

TEST_CASE("static reflection: dispatch precedence between ADL and reflection")
{
    SECTION("a user to_json makes the ADL branch eligible and wins")
    {
        static_assert(refl2::detail::to_adl_branch_eligible_v<json, dispatch::WithMacro>,
                      "the macro-provided to_json must make the ADL branch eligible");
        static_assert(refl2::detail::to_adl_branch_eligible_v<json, dispatch::Plain> == false,
                      "a plain struct must NOT take the ADL branch");
    }

    SECTION("both types are reflectable, so only the precedence decides")
    {
        static_assert(refl2::detail::is_reflectable_struct<false, dispatch::WithMacro>::value, "");
        static_assert(refl2::detail::is_reflectable_struct<false, dispatch::Plain>::value, "");
    }

    SECTION("both still serialize to the same document")
    {
        const json a = dispatch::WithMacro {7};
        const json b = dispatch::Plain {7};
        CHECK(a.dump() == R"({"v":7})");
        CHECK(a.dump() == b.dump());
    }
}
