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
//   6  nested basic_json value          -> j = v / v = j  (native nesting)
//   5  std::optional<T>                 -> T / null (any refl2-serializable T)
//   4  adl branch (strings + non-container adl-able types, i.e. user
//      to_json/from_json, scalars, enums, pair, tuple, ...)
//   3  array-like container             -> per-element recursion
//   2  object-like container (map-like) -> per-value recursion (string keys)
//   1  reflectable struct               -> per-member reflection recursion
//      (base-class members included; private bases / duplicate names are
//      compile errors, see "Inheritance" below)
//   0  anything else                    -> static_assert diagnostic
//
// Containers are deliberately excluded from the adl branch (adl_branch_eligible_v):
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
// Inheritance: base-class members ARE serialized. nonstatic_data_members_of
// sees only direct members, so the member facts are computed from
// subobjects_of (direct base subobjects + direct data members,
// access-filtered, bases first) with a consteval recursion into each base
// via type_of(base_info) — multi-level, depth-first, layout order. Verified
// on this toolchain: the member-access splice v.[:m:] works for members of
// base classes. Three shapes are compile errors instead of silent loss:
// (1) private bases and (2) protected bases under codec<false> — detected
// with is_private / is_protected on the unchecked base list (use codec<true>
// or a to_json); (3) virtual bases — a shared virtual base would flatten its
// members multiple times while C++ has one virtual subobject, so members
// would wrongly duplicate (is_virtual; deduplication not implemented).
// Duplicate member names across the hierarchy (same name in base and derived,
// or diamond inheritance) are also compile errors — they would silently
// overwrite JSON keys. Bit-fields: named bit-fields serialize normally
// (verified; the splice copies the value), unnamed bit-fields are not
// subobjects and are skipped by subobjects_of.
//
// Coverage boundary (what is NOT handled — falls to the priority-0
// static_assert with an actionable message):
//   * unions; std::variant (would need variant_size/variant_alternative
//     integration, ~20 lines, plus a discriminator design)
//   * pointers / self-referential types
//   * ranges with begin/end but no value_type member: NOT excluded — they
//     fall through to the adl branch (nlohmann's range-array path accepts
//     any begin/end type at detection level), so a const-iterable
//     compatible range serializes as an array while an incompatible one
//     (e.g. non-const-iterable) dies with a deep library error rather than
//     the clean static_assert
//   * non-default-constructible container elements (from_json needs
//     value_type{}); map keys other than string-like / arithmetic
//   * C arrays: not supported, but a clean compile error (char[N] still
//     serializes as a string)
//
// std::optional<T> is fully supported via its own dispatch branch
// (priority 5): serializes as T / null for ANY refl2-serializable T
// (reflectable structs, containers, nested json, ...). The is_optional
// exclusion from is_array_like is still required: C++23 added begin()/end()
// + value_type to std::optional, which would otherwise misclassify it as
// array-like and silently serialize optional<int>{5} as "[5]" instead of
// "5" (verified before the fix).
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

namespace detail
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
consteval std::meta::access_context access_context_for(bool unchecked)
{
    if (unchecked)
    {
        return std::meta::access_context::unchecked();
    }
    return std::meta::access_context::unprivileged();
}

// --- compile-time member facts (flat: base members first, then direct) -----
// nonstatic_data_members_of sees only DIRECT members, so a derived type
// would silently drop its base-class members. subobjects_of returns the
// direct base subobjects + direct data members (access-filtered, bases
// first, in declaration order), so inheritance is handled by recursing
// into each base via type_of(base_info) — depth-first, base-before-member,
// matching the object layout order. The member-access splice v.[:m:] works
// for members of base classes too (verified on this toolchain).
consteval std::size_t flat_member_count(std::meta::info cls, std::meta::access_context ctx)
{
    std::size_t n = 0;
    const std::size_t nsub = std::meta::subobjects_of(cls, ctx).size();
    for (std::size_t i = 0; i < nsub; ++i)
    {
        const auto s = std::meta::subobjects_of(cls, ctx)[i];
        if (std::meta::is_base(s))
        {
            n += flat_member_count(std::meta::type_of(s), ctx);
        }
        else
        {
            ++n;
        }
    }
    return n;
}

consteval std::meta::info flat_member(std::meta::info cls, std::meta::access_context ctx, std::size_t I)
{
    const std::size_t nsub = std::meta::subobjects_of(cls, ctx).size();
    for (std::size_t i = 0; i < nsub; ++i)
    {
        const auto s = std::meta::subobjects_of(cls, ctx)[i];
        if (std::meta::is_base(s))
        {
            const std::size_t cnt = flat_member_count(std::meta::type_of(s), ctx);
            if (I < cnt)
            {
                return flat_member(std::meta::type_of(s), ctx, I);
            }
            I -= cnt;
        }
        else if (I == 0)
        {
            return s;
        }
        else
        {
            --I;
        }
    }
    return std::meta::info{};
}

// duplicate member names across the flattened list (same name in a base and
// a derived class, or diamond inheritance) would silently overwrite JSON
// keys -> compile error
template<bool U, typename T>
consteval bool has_duplicate_member_keys()
{
    const std::size_t n = flat_member_count(^^T, access_context_for(U));
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = i + 1; j < n; ++j)
        {
            if (std::meta::identifier_of(flat_member(^^T, access_context_for(U), i)) ==
                std::meta::identifier_of(flat_member(^^T, access_context_for(U), j)))
            {
                return true;
            }
        }
    }
    return false;
}

// private/protected bases: subobjects_of(unprivileged) filters them out, so
// their members would be silently dropped — a compile error under codec<false>.
// is_private / is_protected (on the unchecked base list) give the precise
// access kind for the diagnostic. (codec<true> serializes them via unchecked.)
template<bool U, typename T>
consteval bool has_private_bases()
{
    const std::size_t n = std::meta::bases_of(^^T, std::meta::access_context::unchecked()).size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::meta::is_private(std::meta::bases_of(^^T, std::meta::access_context::unchecked())[i]))
        {
            return true;
        }
    }
    return false;
}

template<bool U, typename T>
consteval bool has_protected_bases()
{
    const std::size_t n = std::meta::bases_of(^^T, std::meta::access_context::unchecked()).size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::meta::is_protected(std::meta::bases_of(^^T, std::meta::access_context::unchecked())[i]))
        {
            return true;
        }
    }
    return false;
}

// virtual bases: subobjects_of reports each virtual-base relationship, so a
// shared virtual base (e.g. diamond with virtual inheritance) would be
// flattened multiple times — but C++ has ONE virtual subobject, so its
// members would wrongly duplicate in the JSON object. Deduplication is not
// implemented; a clean compile error instead (is_virtual verified on this
// toolchain). Checks recursively (the virtual base may be nested).
consteval bool has_virtual_bases_info(std::meta::info cls, std::meta::access_context ctx)
{
    const std::size_t nsub = std::meta::subobjects_of(cls, ctx).size();
    for (std::size_t i = 0; i < nsub; ++i)
    {
        const auto s = std::meta::subobjects_of(cls, ctx)[i];
        if (std::meta::is_base(s))
        {
            if (std::meta::is_virtual(s))
            {
                return true;
            }
            if (has_virtual_bases_info(std::meta::type_of(s), ctx))
            {
                return true;
            }
        }
    }
    return false;
}

template<bool U, typename T>
consteval bool has_virtual_bases()
{
    return has_virtual_bases_info(^^T, access_context_for(U));
}

template<bool U, typename T, typename = void>
struct is_reflectable_struct : std::false_type {};
template<bool U, typename T>
struct is_reflectable_struct<U, T, std::enable_if_t<
    std::is_class<T>::value && !std::is_scalar<T>::value && !std::is_union<T>::value
    && !std::is_array<T>::value && !is_array_like<T>::value && !is_object_like<T>::value>>
{
    // also reflectable when there is an inaccessible base, so the reflect
    // branch's dedicated static_assert (not the generic fallback) fires
    static constexpr bool value =
        (flat_member_count(^^T, access_context_for(U)) > 0)
        || std::meta::has_inaccessible_bases(^^T, access_context_for(U));
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
inline constexpr bool adl_branch_eligible_v =
    is_adl_serializable<B, T>::value
    && !is_object_like<T>::value
    && !(is_array_like<T>::value && !is_string_like<B, T>::value)
    && !(std::is_array<T>::value && !is_string_like<B, T>::value);

template<bool U, typename T>
inline constexpr std::size_t member_count_v = flat_member_count(^^T, access_context_for(U));

template<bool U, typename T, std::size_t I>
inline constexpr std::meta::info member_v = flat_member(^^T, access_context_for(U), I);

template<bool U, typename T, std::size_t I>
inline constexpr std::string_view member_key_v =
    std::meta::identifier_of(member_v<U, T, I>);

// bit-field members cannot bind to a T& (no address), so from_json must
// assign via get<M>() instead of recursing into a T&
template<bool U, typename T, std::size_t I>
inline constexpr bool member_is_bit_field_v =
    std::meta::is_bit_field(member_v<U, T, I>);

// ---------------------------------------------------------------------------
} // namespace detail

// The codec. All member functions are static; Unchecked is the access policy.
// ---------------------------------------------------------------------------
template<bool Unchecked>
struct codec
{
    template<typename T> struct always_false : std::false_type {};

    // ---- to_json side -----------------------------------------------------
    template<typename B, typename T>
    requires detail::is_nested_json<B, T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<6>)
    {
        j = v; // native value nesting; MUST precede adl (string_like trap)
    }

    template<typename B, typename T>
    requires detail::is_optional<T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<5>)
    {
        if (v)
        {
            serialize_one(j, *v); // any refl2-serializable T (struct, container, json, ...)
        }
        else
        {
            j = nullptr; // native nlohmann optional form
        }
    }

    template<typename B, typename T>
    requires detail::adl_branch_eligible_v<B, T>
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<4>)
    {
        nlohmann::adl_serializer<T, B>::to_json(j, v);
    }

    template<typename B, typename T>
    requires detail::is_array_like<T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<3>)
    {
        j = B::array();
        for (auto&& e : v)
        {
            serialize_one(j.emplace_back(), e);
        }
    }

    template<typename B, typename T>
    requires detail::is_object_like<T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<2>)
    {
        j = B::object();
        for (auto&& [k, val] : v)
        {
            serialize_one(j[key_string(k)], val);
        }
    }

    template<typename B, typename T>
    requires detail::is_reflectable_struct<Unchecked, T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<1>)
    {
        static_assert(Unchecked || !detail::has_private_bases<Unchecked, T>(),
                      "refl2: private base class under the unprivileged policy "
                      "would silently drop its members — use codec<true> or add "
                      "a to_json for the type");
        static_assert(Unchecked || !detail::has_protected_bases<Unchecked, T>(),
                      "refl2: protected base class under the unprivileged policy "
                      "would silently drop its members — use codec<true> or add "
                      "a to_json for the type");
        static_assert(!detail::has_virtual_bases<Unchecked, T>(),
                      "refl2: virtual base class not supported — a shared virtual "
                      "base would flatten its members multiple times (C++ has one "
                      "virtual subobject); deduplication is not implemented");
        static_assert(!detail::has_duplicate_member_keys<Unchecked, T>(),
                      "refl2: duplicate member names across the class hierarchy "
                      "(e.g. same name in a base and a derived class, or diamond "
                      "inheritance) would collide in the JSON object");
        reflect_to_json(j, v);
    }

    template<typename B, typename T>
    static void serialize_one_impl(B&, const T&, detail::priority_tag<0>)
    {
        static_assert(always_false<T>::value,
                      "refl2: not serializable — no adl_serializer/to_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a to_json for the "
                      "type (or specialize nlohmann::adl_serializer).");
    }

    template<typename B, typename T>
    static void serialize_one(B& j, const T& v)
    {
        serialize_one_impl(j, v, detail::priority_tag<6>{});
    }

    // ---- from_json side (symmetric) --------------------------------------
    template<typename B, typename T>
    requires detail::is_nested_json<B, T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<6>)
    {
        v = j;
    }

    template<typename B, typename T>
    requires detail::is_optional<T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<5>)
    {
        if (j.is_null())
        {
            v.reset();
        }
        else
        {
            v.emplace();
            deserialize_one(j, *v);
        }
    }

    template<typename B, typename T>
    requires detail::adl_branch_eligible_v<B, T>
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<4>)
    {
        nlohmann::adl_serializer<T, B>::from_json(j, v);
    }

    template<typename B, typename T>
    requires detail::is_array_like<T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<3>)
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
    requires detail::is_object_like<T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<2>)
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
    requires detail::is_reflectable_struct<Unchecked, T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<1>)
    {
        static_assert(Unchecked || !detail::has_private_bases<Unchecked, T>(),
                      "refl2: private base class under the unprivileged policy "
                      "would silently drop its members — use codec<true> or add "
                      "a from_json for the type");
        static_assert(Unchecked || !detail::has_protected_bases<Unchecked, T>(),
                      "refl2: protected base class under the unprivileged policy "
                      "would silently drop its members — use codec<true> or add "
                      "a from_json for the type");
        static_assert(!detail::has_virtual_bases<Unchecked, T>(),
                      "refl2: virtual base class not supported — a shared virtual "
                      "base would flatten its members multiple times (C++ has one "
                      "virtual subobject); deduplication is not implemented");
        static_assert(!detail::has_duplicate_member_keys<Unchecked, T>(),
                      "refl2: duplicate member names across the class hierarchy "
                      "(e.g. same name in a base and a derived class, or diamond "
                      "inheritance) would collide in the JSON object");
        reflect_from_json(j, v);
    }

    template<typename B, typename T>
    static void deserialize_one_impl(const B&, T&, detail::priority_tag<0>)
    {
        static_assert(always_false<T>::value,
                      "refl2: not deserializable — no adl_serializer/from_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a from_json for the "
                      "type (or specialize nlohmann::adl_serializer).");
    }

    template<typename B, typename T>
    static void deserialize_one(const B& j, T& v)
    {
        deserialize_one_impl(j, v, detail::priority_tag<6>{});
    }

    // ---- reflected struct: member loop with pre-built static keys ---------
    template<typename B, typename T, std::size_t I>
    static void serialize_member(B& j, const T& v)
    {
        // one-time-initialized key (any length; thread-safe magic static);
        // no per-call std::string construction and no per-member constructor
        // call site in the hot path
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>);
        serialize_one(j[key], v.[:detail::member_v<Unchecked, T, I>:]);
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
        reflect_to_json_impl(j, v, std::make_index_sequence<detail::member_count_v<Unchecked, T>>{});
    }

    template<typename B, typename T, std::size_t I>
    static void deserialize_member(const B& j, T& v, std::true_type /* bit-field */)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>);
        using M = typename [: std::meta::type_of(detail::member_v<Unchecked, T, I>) :];
        v.[:detail::member_v<Unchecked, T, I>:] = j.at(key).template get<M>();
    }

    template<typename B, typename T, std::size_t I>
    static void deserialize_member(const B& j, T& v, std::false_type /* ordinary */)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>);
        deserialize_one(j.at(key), v.[:detail::member_v<Unchecked, T, I>:]);
    }

    // bit-fields cannot bind to a T& (no address): tag-dispatch instead of
    // if-constexpr-with-splices (GCC 16 does not discard the false branch);
    // explicit <B,T,I> needed — I is not deducible from the tag argument
    template<typename B, typename T, std::size_t I>
    static void deserialize_member(const B& j, T& v)
    {
        deserialize_member<B, T, I>(j, v, std::bool_constant<detail::member_is_bit_field_v<Unchecked, T, I>>{});
    }

    template<typename B, typename T, std::size_t... I>
    static void reflect_from_json_impl(const B& j, T& v, std::index_sequence<I...>)
    {
        (..., deserialize_member<B, T, I>(j, v));
    }

    template<typename B, typename T>
    static void reflect_from_json(const B& j, T& v)
    {
        reflect_from_json_impl(j, v, std::make_index_sequence<detail::member_count_v<Unchecked, T>>{});
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
