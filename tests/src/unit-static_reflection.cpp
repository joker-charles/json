//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (supporting code)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Doctest coverage for the flat-struct reflection extension
// (<nlohmann/reflection.hpp>).
//
// Coverage: plain/nested structs, library container members, object-access
// semantics, and per-type opt-in. Member rename/skip/default annotations,
// enum/variant mapping, binary formats and annotation inspection are out of
// scope for this first slice; the explicit fixtures under tests/static-reflection/
// cover the rejection paths.
//
// The target is registered by tests/CMakeLists.txt for C++26 with -freflection
// and -DJSON_USE_REFLECTION. The JSON_HAS_CPP_26 string below is also what
// cmake/test.cmake scans for when it registers unit-*.cpp files; the guard makes
// a misconfigured build fail loudly instead of compiling zero tests.

#include "doctest_compatibility.h"

#include <nlohmann/reflection.hpp>

#if !defined(JSON_USE_REFLECTION)
    #error "unit-static_reflection.cpp must be compiled with -DJSON_USE_REFLECTION"
#elif !defined(JSON_HAS_CPP_26)
    #error "unit-static_reflection.cpp must be compiled as C++26 with JSON_TEST_KEEP_MACROS defined"
#else

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using nlohmann::json;

namespace flat
{
struct [[=nlohmann::json_serializable{}]] Point
{
    double x{};
    double y{};
};

struct [[=nlohmann::json_serializable{}]] Empty
{
};

struct [[=nlohmann::json_serializable{}]] Outer
{
    int id{};
    Point p{};
    std::vector<Point> ps{};
    std::map<std::string, Point> m{};
    std::optional<int> opt{};
};

struct Plain
{
    int x{};
};

struct [[=nlohmann::json_serializable{}]] WithVector
{
    std::vector<int> v{};
};

struct [[=nlohmann::json_serializable{}]] WithSet
{
    std::set<int> s{};
};

struct [[=nlohmann::json_serializable{}]] WithStringMap
{
    std::map<std::string, int> m{};
};

struct [[=nlohmann::json_serializable{}]] WithArray
{
    std::array<int, 3> arr{};
};

struct [[=nlohmann::json_serializable{}]] NoDefault
{
    int x{};
    NoDefault() = delete;
    explicit NoDefault(int value) : x(value) {}
};
} // namespace flat

// is_annotated_v branch coverage: cv-qualified, reference, non-class and
// incomplete types must not select the reflection serializer.
namespace branch_coverage
{
struct incomplete;

// Annotations on a forward declaration are visible to annotations_of(), so the
// is_complete guard is load-bearing: without it is_annotated_v would be true
// here and the serializer would be selected for an incomplete type.
struct [[=nlohmann::json_serializable{}]] annotated_forward_declaration;

static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<const flat::Point>);
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<volatile flat::Point>);
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<flat::Point&>);
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<int>);
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<incomplete>);
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<annotated_forward_declaration>);

union plain_union
{
    int integer;
    float floating;
};
static_assert(!nlohmann::detail::reflection_flat::is_annotated_v<plain_union>);
} // namespace branch_coverage

namespace macro
{
struct Point
{
    double x;
    double y;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)
} // namespace macro

TEST_CASE("flat-struct reflection: annotated structs serialize")
{
    SECTION("plain annotated struct")
    {
        const json j = flat::Point {1.0, 2.0};
        CHECK(j.dump() == "{\"x\":1.0,\"y\":2.0}");
        const auto p = j.get<flat::Point>();
        CHECK(p.x == doctest::Approx(1.0));
        CHECK(p.y == doctest::Approx(2.0));
    }

    SECTION("empty annotated struct")
    {
        const json j = flat::Empty {};
        CHECK(j.is_object());
        CHECK(j.empty());
        CHECK(j.dump() == "{}");
        CHECK_NOTHROW(static_cast<void>(j.get<flat::Empty>()));
    }

    SECTION("nested positions: member, vector, map, optional")
    {
        const json j = flat::Outer {3, {1.0, 2.0}, {{3.0, 4.0}}, {{"k", {5.0, 6.0}}}, 9};
        CHECK(j.dump() == "{\"id\":3,\"m\":{\"k\":{\"x\":5.0,\"y\":6.0}},\"opt\":9,\"p\":{\"x\":1.0,\"y\":2.0},\"ps\":[{\"x\":3.0,\"y\":4.0}]}");
        const auto o = j.get<flat::Outer>();
        CHECK(o.id == 3);
        CHECK(o.opt == 9);
        CHECK(o.ps.size() == 1);
        CHECK(o.ps[0].x == doctest::Approx(3.0));
    }

    SECTION("a null value resets an optional member")
    {
        const json j = json::parse(R"({"id":1,"p":{"x":1.0,"y":2.0},"ps":[],"m":{},"opt":null})");
        const auto o = j.get<flat::Outer>();
        CHECK(o.id == 1);
        CHECK(!o.opt.has_value());
        CHECK(json(o).at("opt").is_null());
    }

    SECTION("generic BasicJsonType")
    {
        nlohmann::ordered_json oj = flat::Point {1.0, 2.0};
        CHECK(oj.at("x").get<double>() == doctest::Approx(1.0));
        const auto p = oj.get<flat::Point>();
        CHECK(p.x == doctest::Approx(1.0));
        CHECK(p.y == doctest::Approx(2.0));
    }
}

TEST_CASE("flat-struct reflection: byte-identical to the NLOHMANN_DEFINE_TYPE_* path")
{
    CHECK(json(flat::Point {1.0, 2.0}).dump() == json(macro::Point {1.0, 2.0}).dump());
}

TEST_CASE("flat-struct reflection: per-type opt-in and object semantics")
{
    static_assert(std::is_constructible_v<json, flat::Point>);
    static_assert(!std::is_constructible_v<json, flat::Plain>);

    const json j = flat::Point {1.0, 2.0};

    CHECK_THROWS_AS(static_cast<void>(json::parse(R"({"x":1.0})").get<flat::Point>()),
                    json::out_of_range);

    const auto extra = json::parse(R"({"x":1.0,"y":2.0,"extra":3})").get<flat::Point>();
    CHECK(extra.x == doctest::Approx(1.0));
    CHECK(extra.y == doctest::Approx(2.0));
}

TEST_CASE("flat-struct reflection: non-object input and no default ctor")
{
    SECTION("non-object input reports at()'s type error")
    {
        CHECK_THROWS_WITH_AS(static_cast<void>(json(5).get<flat::Point>()),
                             "[json.exception.type_error.304] cannot use at() with number",
                             json::type_error);
    }

    SECTION("from_json into an existing object does not need a default ctor")
    {
        static_assert(!std::is_default_constructible_v<flat::NoDefault>);
        flat::NoDefault value(0);
        nlohmann::adl_serializer<flat::NoDefault>::from_json(json{{"x", 42}}, value);
        CHECK(value.x == 42);
        json::parse(R"({"x":7})").get_to(value);
        CHECK(value.x == 7);
    }
}

TEST_CASE("flat-struct reflection: container from_json requires the matching JSON type")
{
    SECTION("sequential container member rejects null / primitive / object")
    {
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"v":5})").get<flat::WithVector>()),
                             "[json.exception.type_error.302] type must be array, but is number",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"v":null})").get<flat::WithVector>()),
                             "[json.exception.type_error.302] type must be array, but is null",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"v":{"a":1}})").get<flat::WithVector>()),
                             "[json.exception.type_error.302] type must be array, but is object",
                             json::type_error);
        CHECK(json::parse(R"({"v":[1,2,3]})").get<flat::WithVector>().v == std::vector<int> {1, 2, 3});
    }

    SECTION("insert-based container member rejects null / primitive / object")
    {
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"s":7})").get<flat::WithSet>()),
                             "[json.exception.type_error.302] type must be array, but is number",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"s":null})").get<flat::WithSet>()),
                             "[json.exception.type_error.302] type must be array, but is null",
                             json::type_error);
        CHECK(json::parse(R"({"s":[3,1,2]})").get<flat::WithSet>().s == std::set<int> {1, 2, 3});
    }

    SECTION("object member rejects null / primitive / array")
    {
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"m":5})").get<flat::WithStringMap>()),
                             "[json.exception.type_error.302] type must be object, but is number",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"m":null})").get<flat::WithStringMap>()),
                             "[json.exception.type_error.302] type must be object, but is null",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"m":[]})").get<flat::WithStringMap>()),
                             "[json.exception.type_error.302] type must be object, but is array",
                             json::type_error);
        CHECK(json::parse(R"({"m":{"k":1}})").get<flat::WithStringMap>().m.at("k") == 1);
    }

    SECTION("fixed-size array member reports the array type error, not at()'s 304")
    {
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"arr":5})").get<flat::WithArray>()),
                             "[json.exception.type_error.302] type must be array, but is number",
                             json::type_error);
        CHECK_THROWS_WITH_AS(static_cast<void>(json::parse(R"({"arr":{}})").get<flat::WithArray>()),
                             "[json.exception.type_error.302] type must be array, but is object",
                             json::type_error);
    }
}

#endif // JSON_USE_REFLECTION && JSON_HAS_CPP_26
