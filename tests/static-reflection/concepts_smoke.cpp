// concepts_smoke.cpp — verify the C++20 concepts dual-path main-library change
// (to_json.hpp / from_json.hpp / concepts.hpp) selects the SAME overloads as the
// C++11 enable_if path, across standards.
//
// Build (from repo root):
//   g++-16 -std=c++20 -Iinclude -o /tmp/s20 concepts_smoke.cpp && /tmp/s20
//   g++-16 -std=c++11 -Iinclude -o /tmp/s11 concepts_smoke.cpp && /tmp/s11
// Uses the non-amalgamated headers so the #ifdef JSON_HAS_CPP_20 branches are
// actually compiled (single_include would only hold the amalgamated snapshot).
#include <string>
#include <cstdio>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// enum: exercises concepts::enum_type<->is_enum across UDT round-trip
enum class Color : int { Red = 1, Green = 2, Blue = 3 };

// custom struct: exercises object_like/array_like interplay and adl_serializer
struct Point
{
    int x;
    int y;
};
inline void to_json(json& j, const Point& p)
{
    j = json{{"x", p.x}, {"y", p.y}};
}
inline void from_json(const json& j, Point& p)
{
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
}

static int failures = 0;
#define CHECK(cond, label) do { if (!(cond)) { ++failures; std::printf("  [FAIL] %s\n", label); } } while (0)

int main()
{
    // enum to_json / from_json (concepts::enum_type path in C++20, is_enum in C++11)
    {
        json j = Color::Green;
        CHECK(j.is_number_integer() && j.get<int>() == 2, "enum to_json -> number_integer");
        auto c = j.get<Color>();
        CHECK(c == Color::Green, "enum from_json round-trip");
    }

    // string to_json (concepts::string_like path in C++20)
    {
        json j = std::string("hi");
        CHECK(j.is_string() && j.get<std::string>() == "hi", "string to_json");
        json j2 = "str_lit"; // const char[N]
        CHECK(j2.is_string() && j2.get<std::string>() == "str_lit", "c-string to_json");
    }

    // float to_json (concepts::floating_point path in C++20)
    {
        json j = 2.5;
        CHECK(j.is_number_float() && j.get<double>() == 2.5, "float to_json");
        CHECK(std::string(j.dump()) == "2.5", "float dump 2.5");
    }

    // boolean to_json (concepts::boolean_like path in C++20, is_same in C++11)
    {
        json j = true;
        CHECK(j.is_boolean() && j.get<bool>() == true, "boolean to_json (literal)");
        bool b = false;
        json j2 = b;
        CHECK(j2.is_boolean() && j2.get<bool>() == false, "boolean to_json (bool var)");
        json j3 = json(false);
        CHECK(j3.is_boolean() && j3.get<bool>() == false, "boolean basic_json ctor");
    }

    // object to_json (concepts::object_like path in C++20)
    {
        json j = Point{3, 4};
        CHECK(j.is_object(), "object-like to_json (UDT)");
        Point p = j.get<Point>();
        CHECK(p.x == 3 && p.y == 4, "object-like from_json (UDT)");
    }

    // array to_json (concepts::array_like path in C++20)
    {
        std::vector<int> v{1, 2, 3};
        json j(v);
        CHECK(j.is_array() && j.size() == 3, "array-like to_json");
    }

    // basic_json self / ints unaffected
    {
        json a = json::array({nullptr, true, 0, -1, 3.5, "s"});
        CHECK(a.is_array() && a.size() == 6, "basic_json array intact");
        json n = -7;
        CHECK(n.is_number_integer() && n.get<int>() == -7, "int intact");
    }

    std::printf(failures ? "\nCONCEPTS SMOKE: %d FAILURES\n" : "\nCONCEPTS SMOKE PASSED\n", failures);
    return failures ? 1 : 0;
}
