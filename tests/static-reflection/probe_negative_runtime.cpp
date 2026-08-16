// probe_negative_runtime.cpp — runtime ERROR & EDGE paths for the refl2 codec
// that the positive/differential probes deliberately do not exercise:
//
//   (1) struct from_json type-mismatch throws (type_error 304 / 302,
//       out_of_range 403) — the reflect_from_json `j.at(key)` path.
//   (2) json_default: only a MISSING key falls back to T{}; an explicit null
//       value still throws (key present → deserialize null into the member).
//   (3) annotated enum to_json of an OUT-OF-TABLE value returns the first
//       entry's string (the documented "macro to_json returns the first entry
//       on miss" semantics).
//   (4) insert-based container member (std::set) round-trip — regression for
//       the array from_json branch that used to hard-fail on `v[i++]`.
//   (5) fixed-size container member (std::array) round-trip.
//   (6) protected base serialized via codec<true> (base members included;
//       the unprivileged policy rejects it at compile time — see
//       compile_fail/cf_protected_base.cpp).
//
// Build: g++-16 -std=c++26 -freflection -O0 -Iinclude \
//            -o /tmp/pnr tests/static-reflection/probe_negative_runtime.cpp && /tmp/pnr
// ASan: add -O1 -g -fsanitize=address.
#include <array>
#include <cstdio>
#include <set>
#include <string>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

// (1) plain struct
struct Point
{
    double x;
    double y;
};

// (2) json_default member
struct WithDefault
{
    int a{};
    double d [[=refl2::json_default{}]];
};

// (3) annotated enum (red has a json_name; green falls back to its identifier)
enum class Color
{
    red [[=refl2::json_name{"RED"}]] = 1,
    green = 2
};

// (4)(5) container members
struct Containers
{
    std::set<int> s;
    std::array<double, 2> arr;
};

// (6) protected base
struct PBase
{
    int hidden{};
};
struct PDeriv : protected PBase
{
    int own{};
    PDeriv(int h, int o) : PBase{h}, own(o) {}
};

static int g_fail = 0;

static void check(const char* what, bool ok)
{
    std::printf("  %-48s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++g_fail;
    }
}

// expects fn to throw a nlohmann::json::exception with exactly `want_id`
template<typename F>
static bool throws(F fn, int want_id)
{
    try
    {
        fn();
    }
    catch (const nlohmann::json::exception& e)
    {
        return e.id == want_id;
    }
    catch (...)
    {
        return false;
    }
    return false;
}

int main()
{
    std::printf("probe_negative_runtime — error & edge paths\n\n");

    // ---- (1) struct from_json type-mismatch -------------------------------
    std::printf("[1] struct from_json type-mismatch throws\n");
    check("struct from array throws type_error.304",
          throws([] { Point p = json::array({1, 2}).get<Point>(); (void)p; }, 304));
    check("struct from string throws type_error.304",
          throws([] { Point p = json("hi").get<Point>(); (void)p; }, 304));
    check("struct missing key throws out_of_range.403",
          throws([] { Point p = json::object().get<Point>(); (void)p; }, 403));
    check("struct wrong member type throws type_error.302",
          throws([] { Point p = json({{"x", "s"}, {"y", 2.0}}).get<Point>(); (void)p; }, 302));

    // ---- (2) json_default -------------------------------------------------
    std::printf("\n[2] json_default: missing key falls back, explicit null throws\n");
    check("missing key falls back to T{} (a=1, d=0)",
          [] {
              WithDefault w = json::parse("{\"a\":1}").get<WithDefault>();
              return w.a == 1 && w.d == 0.0;
          }());
    check("explicit null value throws type_error.302 (no fallback)",
          throws([] {
              WithDefault w = json::parse("{\"a\":1,\"d\":null}").get<WithDefault>(); (void)w;
          }, 302));

    // ---- (3) enum out-of-table -------------------------------------------
    std::printf("\n[3] annotated enum: out-of-table value -> first entry string\n");
    check("Color::green -> \"green\" (identifier fallback)",
          [] {
              json j = Color::green;
              return j.is_string() && j == "green";
          }());
    check("static_cast<Color>(99) -> \"RED\" (first entry on miss)",
          [] {
              json j = static_cast<Color>(99);
              return j.is_string() && j == "RED";
          }());

    // ---- (4)(5) container members ----------------------------------------
    std::printf("\n[4][5] set / array struct members round-trip\n");
    {
        Containers c;
        c.s = {3, 1, 2};
        c.arr = {1.5, 2.5};
        json j = c;
        check("set member serializes as sorted array", j["s"] == json::array({1, 2, 3}));
        check("array member serializes as array", j["arr"] == json::array({1.5, 2.5}));
        Containers c2 = j.get<Containers>();
        check("set member round-trips", c2.s == c.s);
        check("array member round-trips", c2.arr == c.arr);
    }

    // ---- (6) protected base via codec<true> -------------------------------
    std::printf("\n[6] protected base serialized via codec<true>\n");
    {
        PDeriv p(42, 7);
        json j;
        refl2::codec<true>::to_json(j, p);
        check("protected base member included (unchecked)",
              j == json::parse("{\"hidden\":42,\"own\":7}"));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PROBE PASS" : "PROBE FAIL",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
