// probe_adl_recursion.cpp — feasibility probe for the ADL-aware recursive
// reflection serializer ("refl2", see EVALUATION.md §2.1/§2.2/§2.5).
//
// Verifies the three paths of the design:
//   (A) NESTED — a reflected struct whose members are themselves plain
//       structs, containers of plain structs (vector / map), or nested json
//       values: recursion goes through serialize_one/deserialize_one, so
//       nested plain structs are reflected in turn.
//   (B) ADL — a type that has a user to_json/from_json (free functions found
//       by ADL, or an explicit nlohmann::adl_serializer specialization) is
//       handled by the library's extension machinery, even when it sits
//       inside a reflected struct or inside a container of a reflected
//       struct. Customization WINS over reflection: a reflectable struct
//       that also has a user to_json is serialized by the user's logic.
//   (C) PRIVATE — the serializer defaults to access_context::unprivileged(),
//       so private members are neither serialized nor parsed (no
//       encapsulation break); the refl2::codec<true> variant demonstrates the
//       unchecked() policy (everything) for library-internal use.
//
// Also verifies:
//   * the §2.1 pitfall fix: "recursive serialization of nested class members
//     does NOT fall into nlohmann's ADL overload set" — the codec calls
//     adl_serializer explicitly, so nested user-customized types ARE caught
//     by the extension machinery.
//   * flat-struct output is identical to the v1 naive serializer (no
//     regression on the case v1 could already handle).
//   * nested json values are copied natively (the string_like trap: json's
//     explicit conversion operator makes adl_serializer<json,json>::to_json
//     resolve to the string path and throw on non-strings — so the nested-
//     json branch must precede the adl branch).
//
// Build: g++-16 -std=c++26 -freflection -O0 -Iinclude \
//          -o /tmp/par tests/static-reflection/probe_adl_recursion.cpp && /tmp/par
// ASan: add -O1 -g -fsanitize=address.
#include <charconv>   // from_chars
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <nlohmann/reflection_to_json.hpp>

#include <meta>
#include <optional>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Test types
// ---------------------------------------------------------------------------

// (A) plain structs: nested + containers-of-plain
struct Address
{
    std::string street;
    std::string city;
    int zip{};
};

struct Person
{
    std::string name;
    int age{};
    Address home;
    std::vector<Address> history;
    std::map<std::string, Address> book;
};

// C array member: must route through the library's C-array overloads (b32f811c)
struct CArrayHolder
{
    Address arr[2];
};

// (B) user-customized types
namespace mine
{
struct Date
{
    int y{};
    int m{};
    int d{};
};

// free functions found by ADL — Date is ALSO reflectable (3 public members),
// which is exactly the priority test: the user's to_json must win.
void to_json(json& j, const Date& x)
{
    j = std::to_string(x.y) + "-" + std::to_string(x.m) + "-" + std::to_string(x.d);
}
void from_json(const json& j, Date& x)
{
    std::sscanf(j.get<std::string>().c_str(), "%d-%d-%d", &x.y, &x.m, &x.d);
}
} // namespace mine

// explicit adl_serializer specialization route
struct Celsius
{
    double degrees{};
};
namespace nlohmann
{
template<>
struct adl_serializer<Celsius, void>
{
    static void to_json(json& j, const Celsius& c)
    {
        j = {{"celsius", c.degrees}};
    }
    static void from_json(const json& j, Celsius& c)
    {
        c.degrees = j.at("celsius").get<double>();
    }
};
} // namespace nlohmann

// nested user-customized types inside a reflected struct (the §2.1 pitfall
// fix): `when` is mine::Date (free to_json), `alarms` is vector<mine::Date>,
// `places` is vector<Address> (plain), `extra` is a nested json value.
struct Event
{
    std::string name;
    mine::Date when;
    std::vector<mine::Date> alarms;
    std::vector<Address> places;
    json extra;
};

// (C) private members
class Account
{
  public:
    std::string owner;
    double balance{};

    int secret_id() const
    {
        return secret_id_;
    }

  private:
    int secret_id_{};
};

// private-only: no accessible data members at all
class OnlyPrivate
{
    int a{};
    int b{};
};

// (F) inheritance
struct Base2
{
    std::string base_name;
    int base_id{};
};
struct Mid2 : Base2
{
    std::string mid;
};
struct Derived2 : Mid2
{
    std::string own;
};
struct PrivBase2
{
    int hidden{};
};
struct DerivedPriv : Base2, private PrivBase2
{
    // private base => not an aggregate (C++17); ctor needed
    DerivedPriv(std::string bn, int bid, int h) : Base2{std::move(bn), bid}, PrivBase2{h} {}
    int own{};
};
struct Collide1
{
    int x{};
};
struct Collide2
{
    int x{};
};
struct Dia2 : Collide1, Collide2
{
    int y{};
};
struct VBase
{
    int vb{};
};
struct V1 : virtual VBase
{
    int x1{};
};
struct V2 : virtual VBase
{
    int x2{};
};
struct DiaV : V1, V2
{
    int y{};
};
struct IncompleteT; // forward-declared, never defined
struct BF
{
    int a : 3;
    int : 2;
    int b : 5;
    int c{};
};

// member-count facts used by the [0] classification checks
template<typename T>
inline constexpr std::size_t member_count_public =
    std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()).size();
template<typename T>
inline constexpr std::size_t member_count_all =
    std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()).size();

// ---------------------------------------------------------------------------
// v1 naive serializer (EVALUATION.md §2.1) — flat structs only; used to prove
// refl2 output is identical on the case v1 could already handle.
// ---------------------------------------------------------------------------
namespace v1
{
template<typename T>
void to_json(json& j, const T& v)
{
    j = json::object();
    template for (constexpr auto m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())))
    {
        j[std::string(std::meta::identifier_of(m))] = v.[:m:];
    }
}
} // namespace v1

struct FlatPerson
{
    std::string name;
    int age{};
    double height{};
    std::vector<std::string> tags;
    bool active{};
};

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------
static int g_fail = 0;

static void check(const char* what, bool ok)
{
    std::printf("  %-44s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++g_fail;
    }
}

int main()
{
    using codec_public = refl2::codec<false>;
    using codec_all    = refl2::codec<true>;

    std::printf("probe_adl_recursion — ADL-aware recursive reflection serializer\n\n");

    // ---- trait-level facts ------------------------------------------------
    std::printf("[0] dispatch classification (compile-time)\n");
    check("scalar / string stay on the adl branch",
          refl2::detail::to_adl_branch_eligible_v<json, int> &&
          refl2::detail::to_adl_branch_eligible_v<json, std::string> &&
          refl2::detail::to_adl_branch_eligible_v<json, mine::Date> &&
          refl2::detail::to_adl_branch_eligible_v<json, Celsius>);
    check("array/object containers go to element recursion (never adl)",
          !refl2::detail::to_adl_branch_eligible_v<json, std::vector<std::string>> &&
          !refl2::detail::to_adl_branch_eligible_v<json, std::vector<mine::Date>> &&
          !refl2::detail::to_adl_branch_eligible_v<json, std::vector<Address>> &&
          !refl2::detail::to_adl_branch_eligible_v<json, std::map<std::string, Address>>);
    check("C arrays: char[N] stays on the string path, plain C array -> adl/library C-array overload",
          refl2::detail::to_adl_branch_eligible_v<json, char[5]> &&
          refl2::detail::to_adl_branch_eligible_v<json, Address[2]> &&
          refl2::detail::from_adl_branch_eligible_v<json, Address[2]> &&
          !refl2::detail::is_reflectable_struct<false, Address[2]>::value);
    check("plain struct: adl-viable via the catch-all, but excluded from the adl branch",
          refl2::detail::is_adl_serializable<json, Address>::value &&
          refl2::detail::is_adl_deserializable<json, Address>::value &&
          !refl2::detail::to_adl_branch_eligible_v<json, Address> &&
          !refl2::detail::from_adl_branch_eligible_v<json, Address> &&
          !refl2::detail::to_adl_branch_eligible_v<json, std::vector<Address>> &&
          !refl2::detail::to_adl_branch_eligible_v<json, std::map<std::string, Address>>);
    check("nested json value is adl-viable via the string_like trap, but the",
          refl2::detail::is_adl_serializable<json, json>::value && refl2::detail::is_nested_json<json, json>::value);
    check("  json branch (priority 5) preempts it", true);
    check("private-only class reflectable only with unchecked()",
          !refl2::detail::is_reflectable_struct<false, OnlyPrivate>::value &&
          refl2::detail::is_reflectable_struct<true, OnlyPrivate>::value);
    check("Account: unprivileged counts 2 public members, unchecked 3",
          member_count_public<Account> == 2 && member_count_all<Account> == 3);

    // ---- (A) nested plain structs -----------------------------------------
    std::printf("\n[A] nested plain structs (reflection recursion)\n");
    Address home{"1 Main St", "Springfield", 12345};
    Person p{"Ada", 37, home, {{"2 Old Rd", "Shelbyville", 11111}}, {{"mom", {"4 Elm", "Springfield", 12345}}}};

    json jp;
    codec_public::to_json(jp, p);
    const std::string want_p =
        R"({"age":37,"book":{"mom":{"city":"Springfield","street":"4 Elm","zip":12345}},"history":[{"city":"Shelbyville","street":"2 Old Rd","zip":11111}],"home":{"city":"Springfield","street":"1 Main St","zip":12345},"name":"Ada"})";
    check("serialize nested struct == expected json", jp.dump() == want_p);

    Person p2{};
    codec_public::from_json(jp, p2);
    json jp2;
    codec_public::to_json(jp2, p2);
    check("round-trip: reserialize == original", jp2 == jp);

    // ---- (B) ADL customization priority -----------------------------------
    std::printf("\n[B] ADL customization > reflection\n");
    mine::Date d{2026, 8, 15};
    json jd;
    codec_public::to_json(jd, d);
    check("reflectable struct WITH user to_json -> user form wins",
          jd.dump() == "\"2026-8-15\"");

    json jd_native;
    nlohmann::to_json(jd_native, d);
    check("differential vs native nlohmann (adl type)",
          jd == jd_native && jd_native.dump() == "\"2026-8-15\"");

    Celsius c{21.5};
    json jc;
    codec_public::to_json(jc, c);
    check("explicit adl_serializer specialization wins",
          jc.dump() == R"({"celsius":21.5})");

    Event ev{"launch", {2026, 8, 15}, {{2026, 8, 14}, {2026, 8, 16}},
        {{"3 New Ave", "Capital City", 22222}}, json{{"launchpad", "39A"}}};
    json je;
    codec_public::to_json(je, ev);
    const std::string want_e =
        R"({"alarms":["2026-8-14","2026-8-16"],"extra":{"launchpad":"39A"},"name":"launch","places":[{"city":"Capital City","street":"3 New Ave","zip":22222}],"when":"2026-8-15"})";
    check("nested custom type in reflected struct -> user to_json (pitfall fix)",
          je.dump() == want_e);
    check("nested json value copied natively (string_like trap avoided)",
          je.at("extra").dump() == R"({"launchpad":"39A"})");

    Event ev2{};
    codec_public::from_json(je, ev2);
    json je2;
    codec_public::to_json(je2, ev2);
    check("round-trip with ADL types + containers", je2 == je);

    // ---- (C) private members ----------------------------------------------
    std::printf("\n[C] private members: unprivileged default, unchecked opt-in\n");
    Account acct;
    acct.owner = "ada";
    acct.balance = 42.5;
    // write the private member via unchecked reflection (probe_real_json_value
    // route) so the round-trip-untouched check has a known starting value;
    // declaration order is owner[0], balance[1], secret_id_[2]
    {
        constexpr auto secret_m = std::meta::nonstatic_data_members_of(
                                      ^^Account, std::meta::access_context::unchecked())[2];
        acct.[:secret_m:] = 99;
    }

    json ja;
    codec_public::to_json(ja, acct);
    check("unprivileged: private member omitted",
          ja.dump() == R"({"balance":42.5,"owner":"ada"})");

    Account acct2{};
    codec_public::from_json(ja, acct2);
    check("unprivileged: from_json leaves private member untouched",
          acct2.secret_id() == 0 && acct2.owner == "ada");

    json jau;
    codec_all::to_json(jau, acct);
    check("unchecked: private member included",
          jau.dump() == R"({"balance":42.5,"owner":"ada","secret_id_":99})");

    // ---- (D) std::optional (dedicated branch, never array-like) -----------
    std::printf("\n[D] std::optional: dedicated branch, never array-like\n");
    check("optional classification: never array-like",
          !refl2::detail::is_array_like<std::optional<int>>::value &&
          !refl2::detail::is_array_like<std::optional<Address>>::value);
    {
        json ja1, ja2, jn1, jn2;
        codec_public::to_json(ja1, std::optional<int> {5});
        codec_public::to_json(ja2, std::optional<int> {});
        nlohmann::to_json(jn1, std::optional<int> {5});
        nlohmann::to_json(jn2, std::optional<int> {});
        check("optional<int>: 5 / null, byte-equal to native nlohmann",
              ja1.dump() == "5" && ja2.dump() == "null" && ja1 == jn1 && ja2 == jn2);
        std::optional<int> out;
        codec_public::from_json(ja1, out);
        check("optional<int>: from_json round-trip",
              out.has_value() && *out == 5);
    }
    {
        std::optional<Address> oa{Address{"5 Oak", "Town", 33333}};
        json jo;
        codec_public::to_json(jo, oa);
        check("optional<Address>: serializes as object, not array",
              jo.dump() == R"({"city":"Town","street":"5 Oak","zip":33333})");
        std::optional<Address> oa2;
        codec_public::from_json(jo, oa2);
        check("optional<Address>: from_json round-trip",
              oa2.has_value() && oa2->city == "Town" && oa2->zip == 33333);
        json jnull;
        codec_public::to_json(jnull, std::optional<Address> {});
        check("optional<Address>: empty -> null", jnull.is_null());
        std::optional<Address> oa3{Address{"x", "y", 1}};
        codec_public::from_json(jnull, oa3);
        check("optional<Address>: null -> nullopt", !oa3.has_value());
    }

    // ---- (H) C arrays: library overloads via the adl branch ---------------
    std::printf("\n[H] C arrays: library overloads via the adl branch\n");
    {
        CArrayHolder h{{{"A", "CityA", 1}, {"B", "CityB", 2}}};
        json jh;
        codec_public::to_json(jh, h);
        check("C array member -> array of objects",
              jh.dump() == R"({"arr":[{"city":"CityA","street":"A","zip":1},{"city":"CityB","street":"B","zip":2}]})");
        CArrayHolder h2{};
        codec_public::from_json(jh, h2);
        check("C array member round-trip",
              h2.arr[0].street == "A" && h2.arr[0].city == "CityA" && h2.arr[0].zip == 1 &&
              h2.arr[1].street == "B" && h2.arr[1].city == "CityB" && h2.arr[1].zip == 2);
    }

    // ---- (F) inheritance: base-class members are serialized ---------------
    std::printf("\n[F] inheritance: base members via subobjects_of recursion\n");
    {
        Derived2 d2{"base", 7, "mid", "own"};
        json jd2;
        codec_public::to_json(jd2, d2);
        check("derived serializes base members too (multi-level)",
              jd2.dump() == R"({"base_id":7,"base_name":"base","mid":"mid","own":"own"})");
        Derived2 d3{};
        codec_public::from_json(jd2, d3);
        json jd3;
        codec_public::to_json(jd3, d3);
        check("derived round-trip preserves base members", jd3 == jd2);
    }
    check("private vs protected base classification (is_private/is_protected)",
          refl2::detail::has_private_bases<false, DerivedPriv>() &&
          !refl2::detail::has_protected_bases<false, DerivedPriv>() &&
          !refl2::detail::has_private_bases<false, Derived2>());
    check("virtual base detected (shared virtual subobject guard)",
          refl2::detail::has_virtual_bases<false, DiaV>() &&
          !refl2::detail::has_virtual_bases<false, Derived2>());
    check("duplicate member names across hierarchy detected (compile-time guard)",
          refl2::detail::has_duplicate_member_keys<false, Dia2>() &&
          !refl2::detail::has_duplicate_member_keys<false, Derived2>());
    {
        DerivedPriv dp{"b", 1, 42};
        json jdp;
        codec_all::to_json(jdp, dp);
        check("codec<true>: private base included (unchecked)",
              jdp.dump() == R"({"base_id":1,"base_name":"b","hidden":42,"own":0})");
    }

    // ---- (G) toolchain facts: guards, virtual bases, bit-fields -----------
    std::printf("\n[G] toolchain facts (is_enumerable_type, is_bit_field)\n");
    check("is_enumerable_type pre-checks the bases_of enumeration trap",
          std::meta::is_enumerable_type(^^Derived2) &&
          !std::meta::is_enumerable_type(^^IncompleteT));
    check("bit-field classification: reflectable, not array-like",
          refl2::detail::is_reflectable_struct<false, BF>::value &&
          !refl2::detail::is_array_like<BF>::value &&
          refl2::detail::member_count_v<false, BF> == 3);
    {
        BF bf{};
        bf.a = 3;
        bf.b = 5;
        bf.c = 9;
        json jbf;
        codec_public::to_json(jbf, bf);
        check("named bit-fields serialize; unnamed bit-field skipped",
              jbf.dump() == R"({"a":3,"b":5,"c":9})");
        BF bf2{};
        codec_public::from_json(jbf, bf2);
        check("bit-field from_json round-trip",
              bf2.a == 3 && bf2.b == 5 && bf2.c == 9);
    }

    // ---- (E) v1 compatibility on flat structs ------------------------------
    std::printf("\n[E] flat-struct parity with the v1 naive serializer\n");
    FlatPerson fp{"Bob", 42, 1.75, {"x", "y"}, true};
    json jf1, jf2;
    v1::to_json(jf1, fp);
    codec_public::to_json(jf2, fp);
    check("v1 == refl2 on flat struct", jf1 == jf2 && jf1.dump() == jf2.dump());

    FlatPerson fp2{};
    codec_public::from_json(jf2, fp2);
    json jf3;
    codec_public::to_json(jf3, fp2);
    check("flat round-trip", jf3 == jf2);

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PROBE PASS" : "PROBE FAIL",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
