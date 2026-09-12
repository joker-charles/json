// refl2_codec_old.hpp — historical "before" baseline of the refl2 codec.
// Extracted verbatim from commit 62290f3b (the committed pre-optimization
// version, inline in probe_adl_recursion.cpp at that commit). It is used
// ONLY to measure the runtime "before" baseline (EVALUATION.md §2.2's
// "refl2 v2 (old codec)" column); the current implementation is
// refl2_codec.hpp. Kept in the repo so the old-vs-optimized comparison is
// reproducible; do not treat this as the live codec.
#pragma once
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>
#include <meta>
// The ADL-aware recursive codec (BENCH_ADL_REFLECTION mode).
// ---------------------------------------------------------------------------
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
template<std::size_t N> struct priority_tag : priority_tag < N - 1 > {};
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
             : std::bool_constant < !is_object_like<T>::value > {};

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
struct is_reflectable_struct < U, T, std::enable_if_t <
    std::is_class<T>::value && !std::is_scalar<T>::value && !std::is_union<T>::value
    && !is_array_like<T>::value && !is_object_like<T>::value >>
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
        serialize_one_impl(j, v, priority_tag<5> {});
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
        deserialize_one_impl(j, v, priority_tag<5> {});
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
