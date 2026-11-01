// boundary_matrix.cpp — positive side of the access/type boundary matrix.
//
// The shared core rejects private/protected members, bases, unions and
// anonymous members (see compile_fail/ and namespace_compile_fail/), and as of
// the reference/const guards it rejects reference and const data members with
// an actionable diagnostic. This fixture covers the cases that are supported
// but were previously undocumented or untested:
//
//   * named bit-field: read and round-trip through the public API;
//   * unnamed bit-field: not a subobject, not emitted;
//   * non-default-constructible type: `from_json(j, existing)` works (while
//     `json.get<T>()` keeps the library's default-constructibility rule);
//   * standalone polymorphic type (no base): data members are serialized; the
//     dynamic type is not part of the format.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o boundary_matrix tests/static-reflection/boundary_matrix.cpp
//   ./boundary_matrix

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <type_traits>

using json = nlohmann::json;

namespace cases
{
struct bitfield
{
    int x : 3;
    int y;
};

struct unnamed_bitfield
{
    int : 3;
    int x;
};

struct non_default
{
    explicit non_default(int v) : x(v) {}
    int x;
};

struct polymorphic
{
    virtual ~polymorphic() = default;
    int x{};
};
} // namespace cases

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<cases::bitfield> = true;
template<> inline constexpr bool enable<cases::unnamed_bitfield> = true;
template<> inline constexpr bool enable<cases::non_default> = true;
template<> inline constexpr bool enable<cases::polymorphic> = true;
} // namespace nlohmann::reflection

static_assert(std::is_polymorphic_v<cases::polymorphic>);
static_assert(!std::is_default_constructible_v<cases::non_default>);

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
    std::printf("boundary_matrix — supported access/type cases\n\n");

    std::printf("[1] named bit-field\n");
    const cases::bitfield bf{3, 7};
    const json jb = bf;
    check("bit-field: dump", jb.dump() == R"({"x":3,"y":7})");
    const auto rbf = jb.get<cases::bitfield>();
    check("bit-field: round-trip", rbf.x == 3 && rbf.y == 7);

    std::printf("\n[2] unnamed bit-field is not a subobject\n");
    const cases::unnamed_bitfield ub{5};
    const json ju = ub;
    check("unnamed bit-field: only named member", ju.dump() == R"({"x":5})");
    check("unnamed bit-field: round-trip", ju.get<cases::unnamed_bitfield>().x == 5);

    std::printf("\n[3] non-default-constructible: from_json(existing) works\n");
    const cases::non_default nd{9};
    const json jn = nd;
    check("non-default: to_json", jn.dump() == R"({"x":9})");
    cases::non_default target{0};
    nlohmann::reflection::from_json(jn, target);
    check("non-default: from_json(existing)", target.x == 9);
    // json::get<T>() still requires default-constructibility (library rule).
    static_assert(!std::is_default_constructible_v<cases::non_default>);

    std::printf("\n[4] standalone polymorphic type (no base)\n");
    cases::polymorphic p;
    p.x = 11;
    const json jp = p;
    check("polymorphic: data members only", jp.dump() == R"({"x":11})");
    check("polymorphic: round-trip data", jp.get<cases::polymorphic>().x == 11);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
