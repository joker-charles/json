//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (supporting code)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Reflection / concepts modernization tests (feature/static-reflection):
// verifies that the JSON serialization layer behaves identically whether it is
// dispatched through the C++11 enable_if_t overloads or (under JSON_HAS_CPP_20)
// through the concept-constrained requires-clause overloads added by the
// modernization track. The file deliberately references JSON_HAS_CPP_20 so the
// test harness registers both a -std=c++11 and a -std=c++20 build; running both
// and comparing output proves the dual-path is behavior-preserving.

#include "doctest_compatibility.h"

#include <nlohmann/json.hpp>
using nlohmann::json;

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

// A user-defined type with to_json/from_json (round-trips as an object).
struct json_point
{
    int x;
    int y;
    json_point() = default;
    json_point(int x_, int y_) : x(x_), y(y_) {}
};
inline void to_json(json& j, const json_point& p)
{
    j = json{{"x", p.x}, {"y", p.y}};
}
inline void from_json(const json& j, json_point& p)
{
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
}

TEST_CASE("concepts dual-path serialization is behavior-preserving")
{
    // Build a value exercising every container/UDT category the dual path routes.
    json j;
    j["vec_int"] = std::vector<int> {1, 2, 3};
    j["vec_str"] = std::vector<std::string> {"a", "b"};
    j["deque"]   = std::deque<double> {1.5, 2.5};
    j["list"]    = std::list<long> {10, 20};
    j["set"]     = std::set<int> {1, 5};
    j["obj"]     = std::map<std::string, int> {{"k", 7}};
    j["str"]     = std::string("hi");
    j["bytes"]   = std::vector<std::uint8_t> {1, 2, 250}; // NOT binary -> array
    j["pt"]      = json_point{3, 4};

    const std::string dumped = j.dump();

    SECTION("array/object/string classification is stable")
    {
        CHECK(j["vec_int"].is_array());
        CHECK(j["obj"].is_object());
        CHECK(j["str"].is_string());
        // std::vector<std::uint8_t> must serialize as array, not binary
        CHECK(j["bytes"].is_array());
        CHECK(!j["bytes"].is_binary());
        CHECK(j["pt"].is_object());
    }

    SECTION("round-trip is lossless")
    {
        const json j2 = json::parse(dumped);
        CHECK((j2["vec_int"].get<std::vector<int>>() == std::vector<int> {1, 2, 3}));
        CHECK(j2["str"].get<std::string>() == "hi");
        CHECK(j2["bytes"].get<std::vector<std::uint8_t>>() == std::vector<std::uint8_t> {1, 2, 250});
        const auto p = j2["pt"].get<json_point>();
        CHECK(p.x == 3);
        CHECK(p.y == 4);
    }

#ifdef JSON_HAS_CPP_20
    SECTION("concept-constrained path is active in C++20")
    {
        // A two-parameter concept cannot be called with () as a function; extract
        // its truth via a requires-expression and compare to the library traits.
        constexpr bool s_str = requires { requires nlohmann::detail::concepts::string_like<std::string, json>; };
        constexpr bool s_map = requires { requires nlohmann::detail::concepts::string_like<std::map<std::string, int>, json>; };
        constexpr bool o_map = requires { requires nlohmann::detail::concepts::object_like<std::map<std::string, int>, json>; };
        constexpr bool a_vec = requires { requires nlohmann::detail::concepts::array_like<std::vector<int>, json>; };
        CHECK(s_str);
        CHECK(!s_map);
        CHECK(o_map);
        CHECK(a_vec);
        // The concepts must agree with the library traits they are meant to replace.
        CHECK(s_str == nlohmann::detail::is_compatible_string_type<json, std::string>::value);
        CHECK(s_map == nlohmann::detail::is_compatible_string_type<json, std::map<std::string, int>>::value);
        CHECK(o_map == nlohmann::detail::is_compatible_object_type<json, std::map<std::string, int>>::value);
        CHECK(a_vec == nlohmann::detail::is_compatible_array_type<json, std::vector<int>>::value);
    }
#endif
}
