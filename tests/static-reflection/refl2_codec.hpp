// refl2_codec.hpp — the ADL-aware recursive reflection serializer ("refl2").
// Single source of truth shared by probe_adl_recursion.cpp (feasibility),
// bench_macro_vs_reflection.cpp (compile/binary bench) and
// bench_runtime.cpp (serialize/deserialize throughput).
//
// Implements the recursive dispatch itself, on the PUBLIC nlohmann extension
// point only (nlohmann::adl_serializer); nothing from the library's detail
// internals is used. See EVALUATION.md §2.1/§2.5 for the full design and the
// measured cost.
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
// Containers are deliberately excluded from the adl branch (adl_branch_eligible):
// element recursion produces identical output for the types nlohmann's own
// container paths handle, and additionally covers containers whose elements
// are plain reflected structs. C arrays are excluded too (they would fall
// into nlohmann's C-array to_json path and break on non-constructible
// elements) unless they are string-like (e.g. char[N] serializes as a string).
//
// Performance notes (measured, see EVALUATION.md §2.2):
//   * Member keys are pre-built static std::string objects
//     (function-local statics, one-time init, any length) instead of a
//     per-call std::string(identifier_of(m)) — removes the per-member
//     runtime key construction and the associated codegen. A constexpr
//     std::array<std::string, N> table was tried first and REJECTED: on this
//     toolchain (g++-16, libstdc++ 16) it constant-initializes only for SSO
//     keys (<=15 chars); longer keys fail with "refers to a result of
//     'operator new'".
//   * The member loop is an std::index_sequence pack expansion over consteval
//     member_v<U,T,I> infos (no template for), the pattern this branch
//     verified on GCC 16; there is NO if-constexpr-with-splices anywhere.
//   * The array branch serializes directly into j.emplace_back() instead of a
//     temp json + push_back.
//
// Access policy: codec<false> uses access_context::unprivileged() (public
// members only, the default); codec<true> switches to unchecked() for
// library-internal use.
//
// Coverage boundary (what is NOT handled — falls to the priority-0
// static_assert with an actionable message):
//   * inheritance: nonstatic_data_members_of reports only DIRECTLY-declared
//     members, so base-class members are silently ignored (would need
//     bases_of recursion, ~15 lines)
//   * private/protected base classes; unions; std::variant (would need
//     variant_size/variant_alternative integration, ~20 lines)
//   * std::optional<T>: handled via the adl branch (nlohmann's native
//     optional support) — serializes as T / null, NOT as an array. This
//     needs an explicit is_optional exclusion from is_array_like: C++23
//     added begin()/end() + value_type to std::optional, which would
//     otherwise misclassify it as array-like and silently serialize
//     optional<int>{5} as "[5]" instead of "5" (verified before the fix).
//     optional<PlainStruct> (T not nlohmann-constructible) falls to the
//     priority-0 static_assert.
//   * pointers / self-referential types
//   * ranges with begin/end but no value_type member: NOT excluded — they
//     fall through to the adl branch (nlohmann's range-array path accepts
//     any begin/end type at detection level), so a const-iterable
//     compatible range serializes as an array while an incompatible one
//     (e.g. non-const-iterable) dies with a deep library error rather than
//     the clean static_assert
//   * non-default-constructible container elements (from_json needs
//     value_type{}); map keys other than string-like / arithmetic
#pragma once

#include <charconv>   // from_chars
#include <cstdio>     // fprintf, abort (fixed-size from_json size mismatch)
#include <optional>   // is_optional (C++23 begin/end misclassification guard)
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include <meta>

namespace refl2
{

// ---------------------------------------------------------------------------
// priority tag — overload-ranking dispatch (same idiom nlohmann uses
// internally); no if-constexpr-with-splices (see AGENTS.md §3).
// ---------------------------------------------------------------------------
template<std::size_t N> struct priority_tag : priority_tag<N - 1> {};
template<> struct priority_tag<0> {};

// --- detection traits (self-contained, public API only) --------------------
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

// std::optional is NOT array-like even though C++23 gives it
// begin()/end() + value_type — otherwise optional<int>{5} would silently
// serialize as "[5]" instead of "5"/null (verified before this guard).
template<typename T> struct is_optional : std::false_type {};
template<typename T> struct is_optional<std::optional<T>> : std::true_type {};

template<typename T, typename = void>
struct is_array_like : std::false_type {};
template<typename T>
struct is_array_like<T, std::void_t<
    decltype(std::begin(std::declval<const T&>())),
    decltype(std::end(std::declval<const T&>())),
    typename T::value_type>>
    : std::bool_constant<!is_object_like<T>::value && !is_optional<T>::value> {};

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
    && !std::is_array<T>::value && !is_array_like<T>::value && !is_object_like<T>::value>>
{
    static constexpr bool value =
        (std::meta::nonstatic_data_members_of(^^T, reflect_context(U)).size() > 0);
};

// Eligibility for the adl branch. Containers (array-like, except strings;
// object-like) and non-string C arrays are ALWAYS handled elsewhere:
// nlohmann's from_json detection over-accepts object/array-like types whose
// element/value types are not really gettable (e.g. map<string, PlainStruct>
// — get<pair<const string, PlainStruct>> looks viable to the tuple machinery
// but breaks on instantiation), and its C-array to_json path breaks on
// non-constructible elements. Element recursion is behavior-identical for
// everything nlohmann handles and additionally covers plain reflected structs.
template<typename B, typename T>
inline constexpr bool adl_branch_eligible =
    is_adl_serializable<B, T>::value
    && !is_object_like<T>::value
    && !(is_array_like<T>::value && !is_string_like<B, T>::value)
    && !(std::is_array<T>::value && !is_string_like<B, T>::value);

// --- compile-time member facts (the robust codec pattern, no template for) --
template<bool U, typename T>
inline constexpr std::size_t member_count =
    std::meta::nonstatic_data_members_of(^^T, reflect_context(U)).size();

template<bool U, typename T, std::size_t I>
inline constexpr std::meta::info member_v =
    std::meta::nonstatic_data_members_of(^^T, reflect_context(U))[I];

template<bool U, typename T, std::size_t I>
inline constexpr std::string_view member_key_v =
    std::meta::identifier_of(member_v<U, T, I>);

// ---------------------------------------------------------------------------
// The codec. All member functions are static; Unchecked is the access policy.
// ---------------------------------------------------------------------------
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
            serialize_one(j.emplace_back(), e);
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
        reflect_to_json(j, v);
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
        reflect_from_json(j, v);
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

    // ---- reflected struct: member loop with pre-built static keys ---------
    template<typename B, typename T, std::size_t I>
    static void serialize_member(B& j, const T& v)
    {
        // one-time-initialized key (any length; thread-safe magic static);
        // no per-call std::string construction and no per-member constructor
        // call site in the hot path
        static const std::string key = std::string(member_key_v<Unchecked, T, I>);
        serialize_one(j[key], v.[:member_v<Unchecked, T, I>:]);
    }

    template<typename B, typename T, std::size_t... I>
    static void reflect_to_json_impl(B& j, const T& v, std::index_sequence<I...>)
    {
        j = B::object();
        (..., serialize_member<B, T, I>(j, v));
    }

    template<typename B, typename T>
    static void reflect_to_json(B& j, const T& v)
    {
        reflect_to_json_impl(j, v, std::make_index_sequence<member_count<Unchecked, T>>{});
    }

    template<typename B, typename T, std::size_t I>
    static void deserialize_member(const B& j, T& v)
    {
        static const std::string key = std::string(member_key_v<Unchecked, T, I>);
        deserialize_one(j.at(key), v.[:member_v<Unchecked, T, I>:]);
    }

    template<typename B, typename T, std::size_t... I>
    static void reflect_from_json_impl(const B& j, T& v, std::index_sequence<I...>)
    {
        (..., deserialize_member<B, T, I>(j, v));
    }

    template<typename B, typename T>
    static void reflect_from_json(const B& j, T& v)
    {
        reflect_from_json_impl(j, v, std::make_index_sequence<member_count<Unchecked, T>>{});
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
