// bench_macro_vs_reflection.cpp — compile-time / binary-size benchmark:
// user-facing macro (NLOHMANN_DEFINE_TYPE_INTRUSIVE) vs a generic P2996
// reflection serializer. Three modes, same TU, same standard and flags —
// only the serializer source differs, selected by the -D flag:
//
//   macro mode:   g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 -Iinclude \
//                   -o /tmp/bench_macro bench_macro_vs_reflection.cpp
//   refl (v1):    g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 \
//                   -DBENCH_REFLECTION -Iinclude \
//                   -o /tmp/bench_refl bench_macro_vs_reflection.cpp
//   refl2 (v2):   g++-16 -std=c++26 -freflection -O0 -DBENCH_N=50 \
//                   -DBENCH_ADL_REFLECTION -Iinclude \
//                   -o /tmp/bench_refl2 bench_macro_vs_reflection.cpp
//
//   macro mode  = NLOHMANN_DEFINE_TYPE_INTRUSIVE (the library's own macros)
//   refl (v1)   = the naive generic serializer (EVALUATION.md §2.1): flat
//                 structs only, scalars through assignment/get<>; recursive
//                 member types do NOT work (the documented pitfall).
//   refl2 (v2)  = the ADL-aware recursive serializer: explicit
//                 adl_serializer calls (user customization first), container
//                 element recursion, per-member reflection recursion, and an
//                 unprivileged() access-context default (see
//                 probe_adl_recursion.cpp and EVALUATION.md §2.1/§2.5).
//
// BENCH_N (default 50) = how many person-like structs are actually
// instantiated in main(); all 100 definitions are always present, but unused
// types generate no code, so -DBENCH_N sweeps the marginal cost.
//
// person-like struct: { name, age, height, tags, active } — the five field
// types used for the lines-per-type table in EVALUATION.md §2.2.
//
// Measure with:
//   /usr/bin/time -f "wall=%e s maxrss=%M KB" g++-16 ...   (compile)
//   size <exe> | tail -1                                   (text/data/bss)
//   nm <exe> | wc -l                                       (symbol count)
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(BENCH_REFLECTION) || defined(BENCH_ADL_REFLECTION)
    #include <meta>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// The generic reflection serializer — the one-time cost. The `namespace refl`
// block below is 24 lines; plus 1 `#include <meta>` = the 25-line one-time
// cost used in the lines-per-type arithmetic of EVALUATION.md §2.2 (the
// break-even is at N = 25 types).
// ---------------------------------------------------------------------------
#ifdef BENCH_REFLECTION
namespace refl
{
template<typename BasicJsonType, typename T>
void to_json(BasicJsonType& j, const T& v)
{
    j = BasicJsonType::object();
    template for (constexpr auto m : std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())))
    {
        j[std::string(std::meta::identifier_of(m))] = v.[:m:];
    }
}

template<typename BasicJsonType, typename T>
void from_json(const BasicJsonType& j, T& v)
{
    template for (constexpr auto m : std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())))
    {
        using M = typename [: std::meta::type_of(m) :];
        v.[:m:] = j.at(std::string(std::meta::identifier_of(m))).template get<M>();
    }
}
}
#endif

// ---------------------------------------------------------------------------
// The ADL-aware recursive codec (BENCH_ADL_REFLECTION mode).
// ---------------------------------------------------------------------------
#ifdef BENCH_ADL_REFLECTION
namespace refl2
{
// ---------------------------------------------------------------------------
// refl2 — the ADL-aware recursive codec (BENCH_ADL_REFLECTION mode).
// Reference implementation shared with probe_adl_recursion.cpp; dispatch and
// detection are described there and in EVALUATION.md §2.1. This is the
// "complete recursion + ADL" serializer: adl customization first (strings and
// non-container adl-able types), then array/object container element
// recursion, then per-member reflection recursion for plain structs.
// ---------------------------------------------------------------------------

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
} // namespace refl2
#endif

// ---------------------------------------------------------------------------
// 100 person-like structs, generated by the PERSON() X-macro. In macro mode
// each also gets NLOHMANN_DEFINE_TYPE_INTRUSIVE; in reflection mode the plain
// struct is all the user writes.
// ---------------------------------------------------------------------------
#define PERSON_FIELDS \
    std::string name; \
    int age; \
    double height; \
    std::vector<std::string> tags; \
    bool active;

#ifdef BENCH_REFLECTION
    #define DEFINE_PERSON(i) struct person_##i \
    { \
        PERSON_FIELDS \
    };
#else
    // intrusive: the NLOHMANN_DEFINE_TYPE_INTRUSIVE call lives INSIDE the class
    // body (it defines friend to_json/from_json); per-type user cost = 1 struct
    // line + 1 macro line (the field lines are common to both modes)
    #define DEFINE_PERSON(i) struct person_##i \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_##i, name, age, height, tags, active) \
        PERSON_FIELDS \
    };
#endif

#define PERSON(i) DEFINE_PERSON(i)
#define PERSON(i) DEFINE_PERSON(i)
PERSON(0)
PERSON(1)
PERSON(2)
PERSON(3)
PERSON(4)
PERSON(5)
PERSON(6)
PERSON(7)
PERSON(8)
PERSON(9)
PERSON(10)
PERSON(11)
PERSON(12)
PERSON(13)
PERSON(14)
PERSON(15)
PERSON(16)
PERSON(17)
PERSON(18)
PERSON(19)
PERSON(20)
PERSON(21)
PERSON(22)
PERSON(23)
PERSON(24)
PERSON(25)
PERSON(26)
PERSON(27)
PERSON(28)
PERSON(29)
PERSON(30)
PERSON(31)
PERSON(32)
PERSON(33)
PERSON(34)
PERSON(35)
PERSON(36)
PERSON(37)
PERSON(38)
PERSON(39)
PERSON(40)
PERSON(41)
PERSON(42)
PERSON(43)
PERSON(44)
PERSON(45)
PERSON(46)
PERSON(47)
PERSON(48)
PERSON(49)
PERSON(50)
PERSON(51)
PERSON(52)
PERSON(53)
PERSON(54)
PERSON(55)
PERSON(56)
PERSON(57)
PERSON(58)
PERSON(59)
PERSON(60)
PERSON(61)
PERSON(62)
PERSON(63)
PERSON(64)
PERSON(65)
PERSON(66)
PERSON(67)
PERSON(68)
PERSON(69)
PERSON(70)
PERSON(71)
PERSON(72)
PERSON(73)
PERSON(74)
PERSON(75)
PERSON(76)
PERSON(77)
PERSON(78)
PERSON(79)
PERSON(80)
PERSON(81)
PERSON(82)
PERSON(83)
PERSON(84)
PERSON(85)
PERSON(86)
PERSON(87)
PERSON(88)
PERSON(89)
PERSON(90)
PERSON(91)
PERSON(92)
PERSON(93)
PERSON(94)
PERSON(95)
PERSON(96)
PERSON(97)
PERSON(98)
PERSON(99)
#undef PERSON

// ---------------------------------------------------------------------------
// Preprocessor recursion: expand USE(0) USE(1) ... USE(BENCH_N-1).
// ---------------------------------------------------------------------------
#ifndef BENCH_N
    #define BENCH_N 50
#endif
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define EMPTY()
#define DEFER(id) id EMPTY()
#define EXPAND(...) EXPAND1(EXPAND1(EXPAND1(EXPAND1(__VA_ARGS__))))
#define EXPAND1(...) EXPAND2(EXPAND2(EXPAND2(EXPAND2(__VA_ARGS__))))
#define EXPAND2(...) EXPAND3(EXPAND3(EXPAND3(EXPAND3(__VA_ARGS__))))
#define EXPAND3(...) EXPAND4(EXPAND4(EXPAND4(EXPAND4(__VA_ARGS__))))
#define EXPAND4(...) __VA_ARGS__
// ---- GENERATED BLOCK (USE_ALL_0..USE_ALL_100) — do not hand-edit ----
#define USE_ALL_0()
#define USE_ALL_1() USE_ALL_0() USE(0)
#define USE_ALL_2() USE_ALL_1() USE(1)
#define USE_ALL_3() USE_ALL_2() USE(2)
#define USE_ALL_4() USE_ALL_3() USE(3)
#define USE_ALL_5() USE_ALL_4() USE(4)
#define USE_ALL_6() USE_ALL_5() USE(5)
#define USE_ALL_7() USE_ALL_6() USE(6)
#define USE_ALL_8() USE_ALL_7() USE(7)
#define USE_ALL_9() USE_ALL_8() USE(8)
#define USE_ALL_10() USE_ALL_9() USE(9)
#define USE_ALL_11() USE_ALL_10() USE(10)
#define USE_ALL_12() USE_ALL_11() USE(11)
#define USE_ALL_13() USE_ALL_12() USE(12)
#define USE_ALL_14() USE_ALL_13() USE(13)
#define USE_ALL_15() USE_ALL_14() USE(14)
#define USE_ALL_16() USE_ALL_15() USE(15)
#define USE_ALL_17() USE_ALL_16() USE(16)
#define USE_ALL_18() USE_ALL_17() USE(17)
#define USE_ALL_19() USE_ALL_18() USE(18)
#define USE_ALL_20() USE_ALL_19() USE(19)
#define USE_ALL_21() USE_ALL_20() USE(20)
#define USE_ALL_22() USE_ALL_21() USE(21)
#define USE_ALL_23() USE_ALL_22() USE(22)
#define USE_ALL_24() USE_ALL_23() USE(23)
#define USE_ALL_25() USE_ALL_24() USE(24)
#define USE_ALL_26() USE_ALL_25() USE(25)
#define USE_ALL_27() USE_ALL_26() USE(26)
#define USE_ALL_28() USE_ALL_27() USE(27)
#define USE_ALL_29() USE_ALL_28() USE(28)
#define USE_ALL_30() USE_ALL_29() USE(29)
#define USE_ALL_31() USE_ALL_30() USE(30)
#define USE_ALL_32() USE_ALL_31() USE(31)
#define USE_ALL_33() USE_ALL_32() USE(32)
#define USE_ALL_34() USE_ALL_33() USE(33)
#define USE_ALL_35() USE_ALL_34() USE(34)
#define USE_ALL_36() USE_ALL_35() USE(35)
#define USE_ALL_37() USE_ALL_36() USE(36)
#define USE_ALL_38() USE_ALL_37() USE(37)
#define USE_ALL_39() USE_ALL_38() USE(38)
#define USE_ALL_40() USE_ALL_39() USE(39)
#define USE_ALL_41() USE_ALL_40() USE(40)
#define USE_ALL_42() USE_ALL_41() USE(41)
#define USE_ALL_43() USE_ALL_42() USE(42)
#define USE_ALL_44() USE_ALL_43() USE(43)
#define USE_ALL_45() USE_ALL_44() USE(44)
#define USE_ALL_46() USE_ALL_45() USE(45)
#define USE_ALL_47() USE_ALL_46() USE(46)
#define USE_ALL_48() USE_ALL_47() USE(47)
#define USE_ALL_49() USE_ALL_48() USE(48)
#define USE_ALL_50() USE_ALL_49() USE(49)
#define USE_ALL_51() USE_ALL_50() USE(50)
#define USE_ALL_52() USE_ALL_51() USE(51)
#define USE_ALL_53() USE_ALL_52() USE(52)
#define USE_ALL_54() USE_ALL_53() USE(53)
#define USE_ALL_55() USE_ALL_54() USE(54)
#define USE_ALL_56() USE_ALL_55() USE(55)
#define USE_ALL_57() USE_ALL_56() USE(56)
#define USE_ALL_58() USE_ALL_57() USE(57)
#define USE_ALL_59() USE_ALL_58() USE(58)
#define USE_ALL_60() USE_ALL_59() USE(59)
#define USE_ALL_61() USE_ALL_60() USE(60)
#define USE_ALL_62() USE_ALL_61() USE(61)
#define USE_ALL_63() USE_ALL_62() USE(62)
#define USE_ALL_64() USE_ALL_63() USE(63)
#define USE_ALL_65() USE_ALL_64() USE(64)
#define USE_ALL_66() USE_ALL_65() USE(65)
#define USE_ALL_67() USE_ALL_66() USE(66)
#define USE_ALL_68() USE_ALL_67() USE(67)
#define USE_ALL_69() USE_ALL_68() USE(68)
#define USE_ALL_70() USE_ALL_69() USE(69)
#define USE_ALL_71() USE_ALL_70() USE(70)
#define USE_ALL_72() USE_ALL_71() USE(71)
#define USE_ALL_73() USE_ALL_72() USE(72)
#define USE_ALL_74() USE_ALL_73() USE(73)
#define USE_ALL_75() USE_ALL_74() USE(74)
#define USE_ALL_76() USE_ALL_75() USE(75)
#define USE_ALL_77() USE_ALL_76() USE(76)
#define USE_ALL_78() USE_ALL_77() USE(77)
#define USE_ALL_79() USE_ALL_78() USE(78)
#define USE_ALL_80() USE_ALL_79() USE(79)
#define USE_ALL_81() USE_ALL_80() USE(80)
#define USE_ALL_82() USE_ALL_81() USE(81)
#define USE_ALL_83() USE_ALL_82() USE(82)
#define USE_ALL_84() USE_ALL_83() USE(83)
#define USE_ALL_85() USE_ALL_84() USE(84)
#define USE_ALL_86() USE_ALL_85() USE(85)
#define USE_ALL_87() USE_ALL_86() USE(86)
#define USE_ALL_88() USE_ALL_87() USE(87)
#define USE_ALL_89() USE_ALL_88() USE(88)
#define USE_ALL_90() USE_ALL_89() USE(89)
#define USE_ALL_91() USE_ALL_90() USE(90)
#define USE_ALL_92() USE_ALL_91() USE(91)
#define USE_ALL_93() USE_ALL_92() USE(92)
#define USE_ALL_94() USE_ALL_93() USE(93)
#define USE_ALL_95() USE_ALL_94() USE(94)
#define USE_ALL_96() USE_ALL_95() USE(95)
#define USE_ALL_97() USE_ALL_96() USE(96)
#define USE_ALL_98() USE_ALL_97() USE(97)
#define USE_ALL_99() USE_ALL_98() USE(98)
#define USE_ALL_100() USE_ALL_99() USE(99)
#define USE_UP_TO(n) EXPAND(CAT(USE_ALL_, n)())

static std::size_t g_total = 0;

#if defined(BENCH_ADL_REFLECTION)
    #define USE(i) do \
    { \
        person_##i p{}; \
        json j; \
        refl2::codec<false>::to_json(j, p); \
        g_total += j.dump().size(); \
        refl2::codec<false>::from_json(j, p); \
    } while (0);
#elif defined(BENCH_REFLECTION)
    #define USE(i) do \
    { \
        person_##i p{}; \
        json j; \
        refl::to_json(j, p); \
        g_total += j.dump().size(); \
        refl::from_json(j, p); \
    } while (0);
#else
    #define USE(i) do \
    { \
        person_##i p{}; \
        json j; \
        nlohmann::to_json(j, p); \
        g_total += j.dump().size(); \
        nlohmann::from_json(j, p); \
    } while (0);
#endif

int main()
{
    USE_UP_TO(BENCH_N)
#if defined(BENCH_ADL_REFLECTION)
    std::printf("bench refl2 (adl+recursion) BENCH_N=%d total=%zu\n", BENCH_N, g_total);
#elif defined(BENCH_REFLECTION)
    std::printf("bench refl (v1)  BENCH_N=%d total=%zu\n", BENCH_N, g_total);
#else
    std::printf("bench macro      BENCH_N=%d total=%zu\n", BENCH_N, g_total);
#endif
    return 0;
}
