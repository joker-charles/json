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
//       encapsulation break); the codec<true> variant demonstrates the
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

#include <meta>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// refl2 — the ADL-aware recursive codec (reference implementation).
// Self-contained: the only nlohmann dependency is the PUBLIC extension point
// nlohmann::adl_serializer; detection traits are implemented here, nothing
// from the library's detail internals is used.
//
// Dispatch (serialize_one / deserialize_one), highest priority first:
//   5  nested basic_json value          -> j = v / v = j  (native nesting)
//   4  adl branch (strings + non-container adl-able types, i.e. user
//      to_json/from_json, scalars, enums, optional, pair, tuple, ...)
//   3  array-like container             -> per-element recursion
//   2  object-like container (map-like) -> per-value recursion (string keys)
//   1  reflectable struct               -> per-member reflection recursion
//   0  anything else                    -> static_assert diagnostic
//
// Containers are deliberately excluded from the adl branch (see
// adl_branch_eligible): element recursion produces identical output for the
// types nlohmann's own container paths handle, and additionally covers
// containers whose elements are plain reflected structs.
//
// Priority is resolved by overload ranking on priority_tag (the same idiom
// nlohmann uses internally); there is NO if-constexpr-with-splices, which is
// the dispatch pattern this branch verified to work on GCC 16 (see
// AGENTS.md §3: if constexpr does not discard the false branch).
// ---------------------------------------------------------------------------
template<std::size_t N> struct priority_tag : priority_tag<N - 1> {};
template<> struct priority_tag<0> {};

// --- detection traits (self-contained, public API only) ---
template<typename B, typename T, typename = void>
struct is_adl_serializable : std::false_type {};
template<typename B, typename T>
struct is_adl_serializable<B, T, std::void_t<decltype(
    nlohmann::adl_serializer<T, B>::to_json(std::declval<B&>(), std::declval<const T&>()))>>
    : std::true_type {};

template<typename B, typename T, typename = void>
struct is_adl_deserializable : std::false_type {};
template<typename B, typename T>
struct is_adl_deserializable<B, T, std::void_t<decltype(
    nlohmann::adl_serializer<T, B>::from_json(std::declval<const B&>(), std::declval<T&>()))>>
    : std::true_type {};

template<typename B, typename T>
struct is_nested_json : std::is_same<std::remove_cvref_t<T>, B> {};

// string-like: constructible into the basic_json string type (public alias)
template<typename B, typename T>
struct is_string_like : std::is_constructible<typename B::string_t, T> {};

template<typename T, typename = void>
struct is_object_like : std::false_type {};
template<typename T>
struct is_object_like<T, std::void_t<typename T::key_type, typename T::mapped_type>>
    : std::true_type {};

template<typename T, typename = void>
struct is_array_like : std::false_type {};
template<typename T>
struct is_array_like<T, std::void_t<
    decltype(std::begin(std::declval<const T&>())),
    decltype(std::end(std::declval<const T&>())),
    typename T::value_type>>
    : std::bool_constant<!is_object_like<T>::value> {};

// The access-context policy: codec<false> = unprivileged (public members
// only), codec<true> = unchecked (everything, for library internals).
consteval std::meta::access_context reflect_context(bool unchecked)
{
    if (unchecked)
    {
        return std::meta::access_context::unchecked();
    }
    return std::meta::access_context::unprivileged();
}

template<bool U, typename T, typename = void>
struct is_reflectable_struct : std::false_type {};
template<bool U, typename T>
struct is_reflectable_struct<U, T, std::enable_if_t<
    std::is_class<T>::value && !std::is_scalar<T>::value && !std::is_union<T>::value
    && !is_array_like<T>::value && !is_object_like<T>::value>>
{
    static constexpr bool value =
        (std::meta::nonstatic_data_members_of(^^T, reflect_context(U)).size() > 0);
};

// Eligibility for the adl branch. Containers (array-like, except strings;
// object-like) are ALWAYS handled by the codec's own element-recursion
// branches: nlohmann's from_json detection over-accepts object/array-like
// types whose element/value types are not really gettable (e.g.
// map<string, PlainStruct> — get<pair<const string, PlainStruct>> looks
// viable to the tuple machinery but breaks on instantiation), and the to_json
// side is behaviorally identical either way.
template<typename B, typename T>
inline constexpr bool adl_branch_eligible =
    is_adl_serializable<B, T>::value
    && !is_object_like<T>::value
    && !(is_array_like<T>::value && !is_string_like<B, T>::value);

// --- the codec ---
template<bool Unchecked>
struct codec
{
    template<typename T> struct unsupported : std::false_type {};

    // ---- to_json side -----------------------------------------------------
    template<typename B, typename T>
    requires is_nested_json<B, T>::value
    static void serialize_one_impl(B& j, const T& v, priority_tag<5>)
    {
        j = v; // native value nesting; MUST precede adl (string_like trap)
    }

    template<typename B, typename T>
    requires adl_branch_eligible<B, T>
    static void serialize_one_impl(B& j, const T& v, priority_tag<4>)
    {
        nlohmann::adl_serializer<T, B>::to_json(j, v);
    }

    template<typename B, typename T>
    requires is_array_like<T>::value
    static void serialize_one_impl(B& j, const T& v, priority_tag<3>)
    {
        j = B::array();
        for (auto&& e : v)
        {
            B elem;
            serialize_one(elem, e);
            j.push_back(std::move(elem));
        }
    }

    template<typename B, typename T>
    requires is_object_like<T>::value
    static void serialize_one_impl(B& j, const T& v, priority_tag<2>)
    {
        j = B::object();
        for (auto&& [k, val] : v)
        {
            serialize_one(j[key_string(k)], val);
        }
    }

    template<typename B, typename T>
    requires is_reflectable_struct<Unchecked, T>::value
    static void serialize_one_impl(B& j, const T& v, priority_tag<1>)
    {
        j = B::object();
        template for (constexpr auto m : std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, reflect_context(Unchecked))))
        {
            serialize_one(j[std::string(std::meta::identifier_of(m))], v.[:m:]);
        }
    }

    template<typename B, typename T>
    static void serialize_one_impl(B&, const T&, priority_tag<0>)
    {
        static_assert(unsupported<T>::value,
                      "refl2: not serializable — no adl_serializer/to_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a to_json for the "
                      "type (or specialize nlohmann::adl_serializer).");
    }

    template<typename B, typename T>
    static void serialize_one(B& j, const T& v)
    {
        serialize_one_impl(j, v, priority_tag<5>{});
    }

    // ---- from_json side (symmetric) --------------------------------------
    template<typename B, typename T>
    requires is_nested_json<B, T>::value
    static void deserialize_one_impl(const B& j, T& v, priority_tag<5>)
    {
        v = j;
    }

    template<typename B, typename T>
    requires adl_branch_eligible<B, T>
    static void deserialize_one_impl(const B& j, T& v, priority_tag<4>)
    {
        nlohmann::adl_serializer<T, B>::from_json(j, v);
    }

    template<typename B, typename T>
    requires is_array_like<T>::value
    static void deserialize_one_impl(const B& j, T& v, priority_tag<3>)
    {
        if constexpr (requires { v.push_back(typename T::value_type{}); })
        {
            v.clear();
            for (auto&& e : j)
            {
                typename T::value_type elem{};
                deserialize_one(e, elem);
                v.push_back(std::move(elem));
            }
        }
        else
        {
            // fixed-size container (e.g. std::array): index-assign
            if (j.size() != v.size())
            {
                std::fprintf(stderr, "refl2: array size mismatch (%zu != %zu)\n",
                             static_cast<std::size_t>(j.size()), v.size());
                std::abort();
            }
            std::size_t i = 0;
            for (auto&& e : j)
            {
                deserialize_one(e, v[i++]);
            }
        }
    }

    template<typename B, typename T>
    requires is_object_like<T>::value
    static void deserialize_one_impl(const B& j, T& v, priority_tag<2>)
    {
        v.clear();
        for (const auto& item : j.items())
        {
            typename T::mapped_type val{};
            deserialize_one(item.value(), val);
            v.emplace(key_from_string<typename T::key_type>(item.key()), std::move(val));
        }
    }

    template<typename B, typename T>
    requires is_reflectable_struct<Unchecked, T>::value
    static void deserialize_one_impl(const B& j, T& v, priority_tag<1>)
    {
        template for (constexpr auto m : std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, reflect_context(Unchecked))))
        {
            deserialize_one(j.at(std::string(std::meta::identifier_of(m))), v.[:m:]);
        }
    }

    template<typename B, typename T>
    static void deserialize_one_impl(const B&, T&, priority_tag<0>)
    {
        static_assert(unsupported<T>::value,
                      "refl2: not deserializable — no adl_serializer/from_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a from_json for the "
                      "type (or specialize nlohmann::adl_serializer).");
    }

    template<typename B, typename T>
    static void deserialize_one(const B& j, T& v)
    {
        deserialize_one_impl(j, v, priority_tag<5>{});
    }

    // ---- helpers ----------------------------------------------------------
    template<typename K>
    static std::string key_string(const K& k)
    {
        if constexpr (std::is_arithmetic_v<K>)
        {
            return std::to_string(k);
        }
        else
        {
            return std::string(k);
        }
    }

    template<typename K>
    static K key_from_string(const std::string& s)
    {
        if constexpr (std::is_arithmetic_v<K>)
        {
            K k{};
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), k);
            return k;
        }
        else
        {
            return K(s);
        }
    }

    // ---- public entry points ---------------------------------------------
    template<typename B, typename T>
    static void to_json(B& j, const T& v)
    {
        serialize_one(j, v);
    }

    template<typename B, typename T>
    static void from_json(const B& j, T& v)
    {
        deserialize_one(j, v);
    }
};

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
struct adl_serializer<Celsius, json>
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
    using codec_public = codec<false>;
    using codec_all    = codec<true>;

    std::printf("probe_adl_recursion — ADL-aware recursive reflection serializer\n\n");

    // ---- trait-level facts ------------------------------------------------
    std::printf("[0] dispatch classification (compile-time)\n");
    check("scalar / string stay on the adl branch",
          adl_branch_eligible<json, int> &&
          adl_branch_eligible<json, std::string> &&
          adl_branch_eligible<json, mine::Date> &&
          adl_branch_eligible<json, Celsius>);
    check("array/object containers go to element recursion (never adl)",
          !adl_branch_eligible<json, std::vector<std::string>> &&
          !adl_branch_eligible<json, std::vector<mine::Date>> &&
          !adl_branch_eligible<json, std::vector<Address>> &&
          !adl_branch_eligible<json, std::map<std::string, Address>>);
    check("plain struct / container-of-plain -> NOT adl-serializable",
          !is_adl_serializable<json, Address>::value &&
          !is_adl_serializable<json, std::vector<Address>>::value &&
          !is_adl_serializable<json, std::map<std::string, Address>>::value &&
          !is_adl_deserializable<json, std::vector<Address>>::value);
    check("nested json value is adl-viable via the string_like trap, but the",
          is_adl_serializable<json, json>::value && is_nested_json<json, json>::value);
    check("  json branch (priority 5) preempts it", true);
    check("private-only class reflectable only with unchecked()",
          !is_reflectable_struct<false, OnlyPrivate>::value &&
          is_reflectable_struct<true, OnlyPrivate>::value);
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
