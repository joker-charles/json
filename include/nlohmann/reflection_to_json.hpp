// reflection_to_json.hpp — P2996 static-reflection-driven serialization for
// nlohmann::json user-defined types (the M5 milestone of the
// feature/static-reflection branch).
//
// WHAT THIS IS
//   Two constrained catch-all overloads in nlohmann::detail::{to,from}_json
//   (active only when the compiler provides C++26 static reflection:
//   g++-16 -std=c++26 -freflection defines __cpp_impl_reflection and
//   __cpp_lib_reflection; nothing else sees this file) make ANY reflectable
//   struct serialize/deserialize automatically:
//
//       struct Point { double x; double y; };
//       json j = Point{1.0, 2.0};   // {"x":1.0,"y":2.0}  — no macro, no to_json
//       auto p = j.get<Point>();    // round-trip
//
//   Member-level annotations express the semantics of the
//   NLOHMANN_DEFINE_TYPE_* macro family (see M5_REFLECTION_TO_JSON.md for the
//   full mapping and the deliberate semantic differences):
//
//       struct S
//       {
//           int id;                                  // default: key = member name
//           [[=refl2::json_name{"display"}]] int a;  // _WITH_NAMES
//           [[=refl2::json_ignore{}]] int secret;    // partial member list
//           [[=refl2::json_default{}]] int d;        // _WITH_DEFAULT (from_json)
//       };
//
// DESIGN (see docs/static-reflection/M5_REFLECTION_TO_JSON.md for details)
//   * The dispatch is the refl2 codec (moved from tests/static-reflection/
//     refl2_codec.hpp — this header is now its single source of truth), an
//     ADL-aware recursive serializer: nested json > std::optional > adl
//     branch > array-like > object-like > reflected struct > static_assert.
//   * Customization precedence: hand-written to_json/from_json (ADL), an
//     explicit adl_serializer<T, void> specialization, and the
//     NLOHMANN_DEFINE_TYPE_* macros all WIN over reflection. Two mechanisms:
//       - the catch-all requires-clause excludes "has user free to_json via
//         ADL" with a NON-CIRCULAR probe (an ADL-detection idiom in a private
//         namespace: ordinary lookup never sees nlohmann::detail overloads,
//         so it cannot observe the catch-all itself);
//       - adl_serializer<T, void> specializations are reached directly by the
//         library's constructor / get<T> machinery (top level) and by the
//         codec's adl branch (nested), which skips the reflect branch for
//         types whose serializer is NOT the primary template
//         (adl_serializer_is_primary: `&adl_serializer<T,void>::template
//         to_json<B,T>` is only addressable for the primary template, whose
//         to_json is a member template).
//   * CIRCULARITY (the reason for the probes): in a reflection build the
//     catch-all makes adl_serializer<T, void>::to_json VALID for plain
//     structs through the CPO chain (has_to_json -> is_compatible_type ->
//     constructor -> adl_serializer -> ::nlohmann::to_json -> to_json_fn ->
//     detail::to_json). If the codec's own adl branch used that probe it
//     would recurse infinitely at compile time. The codec therefore excludes
//     catch-all-eligible primary-template types from its adl branch.
//   * from_json exclusion set must NOT use is_getable (it probes j.get<T>()
//     which goes through the catch-all — the same circularity); containers
//     are excluded by structure (is_array_like / is_object_like / is_optional
//     / is_string_like_from) instead.
//   * Access policy: the catch-all uses codec<false> (unprivileged(), public
//     members only — no implicit encapsulation break). codec<true>
//     (unchecked(), everything) is the explicit opt-in for private members,
//     mirroring the INTRUSIVE macro's friend privilege.
//   * Compile-time guards (from refl2): private/protected bases, virtual
//     bases (duplicate flattening), and duplicate JSON keys across the
//     hierarchy (including json_name annotation collisions) are compile
//     errors with actionable messages instead of silent data loss.
//
// BUILD
//   Only compiles under g++-16 -std=c++26 -freflection (it #errors
//   otherwise). Included by nlohmann/detail/conversions/{to,from}_json.hpp
//   behind `#if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)`.
//   This header must NOT include <nlohmann/json.hpp> (circular: it is part of
//   the json.hpp include chain); it only needs adl_serializer.hpp and the
//   meta traits, both already included before it.
//
// COVERAGE BOUNDARY (falls to the priority-0 static_assert):
//   unions; pointers / self-referential types; non-default-
//   constructible types on the from_json side (the T& form requires an
//   existing object; has_non_default_from_json is not provided) — and
//   std::variant ALTERNATIVES must be default-constructible on the
//   from_json side (emplace<I>()); C arrays (char[N] still serializes as
//   a string via the existing overloads).
//   std::variant itself IS supported (M7): the dedicated codec branch
//   serializes the active alternative as {"index": N, "value": <alt>}.
//   See docs/static-reflection/M7_VARIANT.md.

#pragma once

#if !defined(__cpp_impl_reflection) || !defined(__cpp_lib_reflection)
    #error "reflection_to_json.hpp requires C++26 static reflection (g++-16 -std=c++26 -freflection)"
#endif

#include <array>      // array (compile-time key storage)
#include <charconv>   // from_chars
#include <cstdio>     // fprintf, abort (fixed-size from_json size mismatch)
#include <forward_list> // is_library_dedicated_array (front_inserter from_json)
#include <optional>   // is_optional (C++23 begin/end misclassification guard)
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <valarray>   // is_library_dedicated_array (resize from_json)
#include <variant>    // is_variant, variant_alternative_t (M7)

#include <nlohmann/adl_serializer.hpp>
#include <nlohmann/detail/exceptions.hpp>       // JSON_THROW, type_error (enum from_json)
#include <nlohmann/detail/meta/cpp_future.hpp>   // is_detected_exact
#include <nlohmann/detail/meta/type_traits.hpp>  // is_compatible_*, is_specialization_of, ...

#include <meta>

namespace refl2
{

// ---------------------------------------------------------------------------
// Member annotations (P3394R4 value annotations, member-level only — GCC 16
// ignores type-level annotations).
//   json_name  — structural value annotation: [[=refl2::json_name{"key"}]],
//                overrides the JSON key for this member (extract-read).
//   json_ignore — marker: the member is excluded from both directions.
//   json_default — marker: from_json falls back to T{}.member when the key is
//                missing (the _WITH_DEFAULT macro semantics; requires T to be
//                default-constructible, same as the macro).
// ---------------------------------------------------------------------------
struct json_name
{
    // Fixed-size char array (not std::string / std::string_view / const char*):
    // a structural annotation type must have public non-static data members
    // of structural type — libstdc++'s string/string_view members are private
    // ("does not have structural type"), a const char* member makes
    // meta::extract fail ("reflect_constant failed"), and string literals can
    // never be template arguments. A char array is structural AND extractable
    // (verified). The consteval constructor turns an over-long key into a
    // clear static_assert instead of the opaque aggregate "initializer-string
    // for char[64] too long" error (verified: it does not break structuralness
    // or meta::extract). Keys longer than 63 chars are the documented limit.
    char value[64]{};

    template<std::size_t N>
    consteval json_name(const char (&s)[N])
    {
        static_assert(N <= 64, "refl2::json_name key too long (max 63 chars, incl. NUL)");
        for (std::size_t i = 0; i < N; ++i)
        {
            value[i] = s[i];
        }
    }
};
struct json_ignore {};
struct json_default {};

namespace detail
{

// ---------------------------------------------------------------------------
// Non-circular ADL probes for user-customization detection.
// Declared in a private namespace: the unqualified to_json/from_json call is
// resolved by ADL ONLY (ordinary lookup finds nothing here), so it detects
// the user's free functions without ever consulting nlohmann::detail's own
// overload set — which is exactly what makes it immune to the catch-all.
// ---------------------------------------------------------------------------
namespace adl_probe
{
template<typename B, typename T>
auto has_to_json(int) -> decltype(to_json(std::declval<B&>(), std::declval<const T&>()), std::true_type {});
template<typename B, typename T>
std::false_type has_to_json(...);

template<typename B, typename T>
auto has_from_json(int) -> decltype(from_json(std::declval<const B&>(), std::declval<T&>()), std::true_type {});
template<typename B, typename T>
std::false_type has_from_json(...);
} // namespace adl_probe

// Is the entity declared inside the library's own namespace tree (anything
// under the top-level "nlohmann" namespace — identity_tag, iteration_proxy_value,
// json_pointer, iterators, ...)? Such types are in the associated namespace of
// nlohmann::detail, so an ADL probe would see the library's OWN overloads AND
// the catch-all — re-entering the catch-all's constraint and recursing
// (observed via the has_non_default_from_json -> identity_tag chain). The
// catch-all must never participate for them, and the probes must never fire
// for them.
consteval bool in_json_namespace(std::meta::info entity)
{
    std::meta::info p = entity;
    while (std::meta::has_parent(p))
    {
        p = std::meta::parent_of(p);
        // the global namespace has no identifier — guard before querying
        if (std::meta::has_identifier(p) && std::meta::identifier_of(p) == "nlohmann")
        {
            return true;
        }
    }
    // template arguments may carry the library namespace too
    // (initializer_list<json_ref<json>>, vector<basic_json>, ...): ADL sees
    // the namespaces of template arguments, so the probe could re-enter the
    // catch-all constraint through them.
    if (std::meta::has_template_arguments(entity))
    {
        const std::size_t n = std::meta::template_arguments_of(entity).size();
        for (std::size_t i = 0; i < n; ++i)
        {
            if (in_json_namespace(std::meta::template_arguments_of(entity)[i]))
            {
                return true;
            }
        }
    }
    return false;
}

template<typename B, typename T, typename = void>
struct has_user_to_json : decltype(adl_probe::has_to_json<B, T>(0)) {};
// library-internal types: false WITHOUT probing (the probe would recurse)
template<typename B, typename T>
struct has_user_to_json<B, T, std::enable_if_t<in_json_namespace(^^T)>> : std::false_type {};

template<typename B, typename T, typename = void>
struct has_user_from_json : decltype(adl_probe::has_from_json<B, T>(0)) {};
// library-internal types: false WITHOUT probing (the probe would recurse)
template<typename B, typename T>
struct has_user_from_json<B, T, std::enable_if_t<in_json_namespace(^^T)>> : std::false_type {};

// Is adl_serializer<T, void> the PRIMARY template (i.e. NOT user-specialized)?
// The primary's to_json is a member TEMPLATE, so
// `&adl_serializer<T, void>::template to_json<B, T>` is only addressable for
// the primary. A user specialization with the documented shape (a non-template
// static to_json/from_json) is not. Residual gap (documented): a
// specialization that declares a member template to_json with the same shape
// is indistinguishable.
template<typename B, typename T, typename = void>
struct adl_serializer_is_primary : std::false_type {};
template<typename B, typename T>
struct adl_serializer_is_primary<B, T, std::void_t<
decltype(&nlohmann::adl_serializer<T, void>::template to_json<B, T>)>>
    : std::true_type {};

// ---------------------------------------------------------------------------
// Annotation reading (all verified on g++-16, see modern-cpp skill):
//   * annotations_of(info) is transient — subscript/size directly on the
//     call; the `const auto s = ...` locals below are fine inside a consteval
//     function (same pattern as the flat-member walker).
//   * the annotation type is cv/ref-qualified — strip with remove_cvref_t
//     before matching.
//   * annotation infos cannot appear in a splice expression — json_name is
//     read via extract<json_name>(ann).value (the structural-value route).
// ---------------------------------------------------------------------------
// Annotation reading (query-domain only — NO splices and NO template for):
// a splice needs the annotated entity as a constant expression, which a
// consteval function parameter is not, and binding a reflection vector
// element to a local constexpr is rejected ("refers to a result of operator
// new"). The query-domain form below — direct subscripting of the transient
// annotations_of call + meta::remove_cvref (the annotation type is
// cv/ref-qualified) + meta::is_same_type + extract — has no such
// requirements and takes the member as a plain function parameter (verified
// pattern: refl2's flat-member walker).
template<typename Ann>
consteval bool has_annotation(std::meta::info member)
{
    const std::size_t n = std::meta::annotations_of(member).size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::meta::is_same_type(
                    std::meta::remove_cvref(std::meta::type_of(std::meta::annotations_of(member)[i])),
                    ^^Ann))
        {
            return true;
        }
    }
    return false;
}

// The JSON key for a member: json_name annotation value, else identifier_of.
// Returns a VALUE-COPIED char array: a string_view would point into the
// temporary annotation object returned by extract (not a constant
// expression, and dangling) — the array is a compile-time value.
consteval std::array<char, 64> member_json_key(std::meta::info member)
{
    std::array<char, 64> out{};
    const std::size_t n = std::meta::annotations_of(member).size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::meta::is_same_type(
                    std::meta::remove_cvref(std::meta::type_of(std::meta::annotations_of(member)[i])),
                    ^^json_name))
        {
            // bind the WHOLE extracted value, not its array subobject — the
            // temporary annotation object dies at the end of the full
            // expression ("accessing <anonymous> outside its lifetime")
            const auto extracted = std::meta::extract<json_name>(std::meta::annotations_of(member)[i]);
            for (std::size_t k = 0; k < out.size(); ++k)
            {
                out[k] = extracted.value[k];
            }
            return out;
        }
    }
    const auto id = std::meta::identifier_of(member);
    const std::size_t m = id.size() < out.size() ? id.size() : out.size();
    for (std::size_t k = 0; k < m; ++k)
    {
        out[k] = id[k];
    }
    return out;
}

// ---------------------------------------------------------------------------
// String-like probes mirroring the library's own overload probes EXACTLY
// (the M4 "exact probe targets" discipline — a semantic restatement drifts).
//   to_json string overload: is_constructible<string_t, CompatibleString>
//   from_json string overload: is_assignable<StringType&, const string_t>
//       && is_detected_exact<value_type, value_type_t, StringType>
// ---------------------------------------------------------------------------
template<typename B, typename T>
struct is_string_like_to : std::is_constructible<typename B::string_t, T> {};

template<typename B, typename T>
struct is_string_like_from
{
    static constexpr bool value =
        std::is_assignable<T&, const typename B::string_t>::value
        && nlohmann::detail::is_detected_exact<typename B::string_t::value_type,
        nlohmann::detail::value_type_t, T>::value;
};

// std::optional is NOT array-like even though C++23 gives it
// begin()/end() + value_type — otherwise optional<int>{5} would silently
// serialize as "[5]" instead of "5"/null (verified before this guard).
template<typename T> struct is_optional : std::false_type {};
template<typename T> struct is_optional<std::optional<T>> : std::true_type {};

// std::variant (M7): the dedicated codec branch serializes the active
// alternative as {"index": N, "value": <alternative>} (oneof semantics).
template<typename T> struct is_variant : std::false_type {};
template<typename... Ts> struct is_variant<std::variant<Ts...>> : std::true_type {};

template<typename B, typename T>
struct is_nested_json : std::is_same<std::remove_cvref_t<T>, B> {};

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
             : std::bool_constant < !is_object_like<T>::value && !is_optional<T>::value > {};

// Types that are array_like but which the library has DEDICATED overloads for
// (not the generic array path). The refl2 array branch must NOT intercept
// them — it would mis-handle each one:
//   * B::binary_t        -> would serialize as a number array, not
//                           {"bytes":[...],"subtype":...}
//   * std::forward_list  -> from_json has no size/operator[] (needs
//                           front_inserter; the array branch's three-way
//                           push_back/insert/fixed-size dispatch all miss)
//   * std::valarray      -> from_json has no push_back/insert; the fixed-size
//                           branch aborts on the default-constructed size 0
//   * std::u8string      -> char8_t is not constructible into string_t, so the
//                           string-like probe misses it and the array branch
//                           would emit a number array instead of the library's
//                           dedicated char8_t-string overload (a string)
// They route through the adl branch (the library's own overloads) instead.
// char8_t string (std::u8string) — SFINAE-friendly probe (the value_type
// access must be guarded so non-string types don't hard-error).
template<typename T, typename = void>
struct is_char8_string : std::false_type {};
template<typename T>
struct is_char8_string<T, std::void_t<typename T::value_type>>
    : std::bool_constant <
    nlohmann::detail::is_specialization_of<std::basic_string, T>::value
    && std::is_same_v<typename T::value_type, char8_t> > {};

template<typename B, typename T>
inline constexpr bool is_library_dedicated_array =
    std::is_same<std::remove_cvref_t<T>, typename B::binary_t>::value
    || nlohmann::detail::is_specialization_of<std::forward_list, std::remove_cvref_t<T>>::value
    || nlohmann::detail::is_specialization_of<std::valarray, std::remove_cvref_t<T>>::value
    || is_char8_string<std::remove_cvref_t<T>>::value;

// --- adl_serializer-based detection (the library's own customization
// surface is adl_serializer<T, void> — json_serializer<T, void>) -----------
template<typename B, typename T, typename = void>
struct is_adl_serializable : std::false_type {};
template<typename B, typename T>
struct is_adl_serializable<B, T, std::void_t<decltype(
    nlohmann::adl_serializer<T, void>::to_json(std::declval<B&>(), std::declval<const T&>()))>>
    : std::true_type {};

template<typename B, typename T, typename = void>
struct is_adl_deserializable : std::false_type {};
template<typename B, typename T>
struct is_adl_deserializable<B, T, std::void_t<decltype(
    nlohmann::adl_serializer<T, void>::from_json(std::declval<const B&>(), std::declval<T&>()))>>
    : std::true_type {};

// The access-context policy: codec<false> = unprivileged (public members
// only, the safe default), codec<true> = unchecked (everything, the explicit
// opt-in for private members — the INTRUSIVE macro's friend privilege).
consteval std::meta::access_context access_context_for(bool unchecked)
{
    if (unchecked)
    {
        return std::meta::access_context::unchecked();
    }
    return std::meta::access_context::unprivileged();
}

// ---------------------------------------------------------------------------
// Compile-time member facts.
// nonstatic_data_members_of sees only DIRECT members, so a derived type
// would silently drop its base-class members. subobjects_of returns the
// direct base subobjects + direct data members (access-filtered, bases
// first, in declaration order), so inheritance is handled by recursing
// into each base via type_of(base_info) — depth-first, base-before-member,
// matching the object layout order. The member-access splice v.[:m:] works
// for members of base classes too (verified on this toolchain).
// Members annotated [[=refl2::json_ignore{}]] are excluded (the "partial
// member list" semantics); the count/index walkers below are the EFFECTIVE
// (post-annotation) ones.
// ---------------------------------------------------------------------------
consteval std::size_t effective_member_count(std::meta::info cls, std::meta::access_context ctx)
{
    std::size_t n = 0;
    const std::size_t nsub = std::meta::subobjects_of(cls, ctx).size();
    for (std::size_t i = 0; i < nsub; ++i)
    {
        const auto s = std::meta::subobjects_of(cls, ctx)[i];
        if (std::meta::is_base(s))
        {
            n += effective_member_count(std::meta::type_of(s), ctx);
        }
        else if (!has_annotation<json_ignore>(s))
        {
            ++n;
        }
    }
    return n;
}

consteval std::meta::info effective_member(std::meta::info cls, std::meta::access_context ctx, std::size_t I)
{
    const std::size_t nsub = std::meta::subobjects_of(cls, ctx).size();
    for (std::size_t i = 0; i < nsub; ++i)
    {
        const auto s = std::meta::subobjects_of(cls, ctx)[i];
        if (std::meta::is_base(s))
        {
            const std::size_t cnt = effective_member_count(std::meta::type_of(s), ctx);
            if (I < cnt)
            {
                return effective_member(std::meta::type_of(s), ctx, I);
            }
            I -= cnt;
        }
        else if (has_annotation<json_ignore>(s))
        {
            // excluded member — skip
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

// duplicate JSON keys across the flattened effective member list (same name
// in a base and a derived class, diamond inheritance, or json_name
// annotation collisions) would silently overwrite JSON keys -> compile error
template<bool U, typename T>
consteval bool has_duplicate_member_keys()
{
    const std::size_t n = effective_member_count(^^T, access_context_for(U));
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = i + 1; j < n; ++j)
        {
            if (member_json_key(effective_member(^^T, access_context_for(U), i)) ==
                    member_json_key(effective_member(^^T, access_context_for(U), j)))
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
struct is_reflectable_struct < U, T, std::enable_if_t <
    std::is_class<T>::value && !std::is_scalar<T>::value && !std::is_union<T>::value
    && !std::is_array<T>::value && !is_array_like<T>::value && !is_object_like<T>::value >>
{
    // also reflectable when there is an inaccessible base, so the reflect
    // branch's dedicated static_assert (not the generic fallback) fires
    static constexpr bool value =
        (effective_member_count(^^T, access_context_for(U)) > 0)
        || std::meta::has_inaccessible_bases(^^T, access_context_for(U));
};

// ---------------------------------------------------------------------------
// Catch-all eligibility — the exact negative mirror of the existing
// detail::to_json / detail::from_json overload sets (each exclusion reuses
// the library's own probe for the overload it mirrors), plus the
// user-customization exclusions (non-circular) and the refl2 reflectable
// shape. A type is eligible iff NO existing overload handles it, the user
// has not customized it, and it is a reflectable struct.
// ---------------------------------------------------------------------------
template<typename B, typename T>
struct to_json_eligible
{
    static constexpr bool value =
        !in_json_namespace(^^T)                 // library-internal types (identity_tag, ...) never
        && std::is_class<T>::value              // participate — MUST precede the ADL probe
        && !std::is_union<T>::value
        && !std::is_scalar<T>::value            // arithmetic, enum, pointer, ...
        && !std::is_array<T>::value
        && !nlohmann::detail::is_basic_json<T>::value
        && !is_string_like_to<B, T>::value      // string overload probe
        && !nlohmann::detail::is_compatible_array_type<B, T>::value
        && !nlohmann::detail::is_compatible_object_type<B, T>::value
        && !nlohmann::detail::is_compatible_binary_type<B, T>::value
#if JSON_HAS_RANGES && !defined(__MINGW32__)
        && !nlohmann::detail::is_compatible_range_view<T>::value
#endif
        && !nlohmann::detail::is_constructible_tuple<B, T>::value  // structured-binding structs
        && !nlohmann::detail::is_specialization_of<std::pair, T>::value
        && !nlohmann::detail::is_specialization_of<std::tuple, T>::value
#if JSON_HAS_FILESYSTEM
        && !std::is_same<std::remove_cvref_t<T>, std::filesystem::path>::value
#endif
#if JSON_HAS_EXPERIMENTAL_FILESYSTEM
        && !std::is_same<std::remove_cvref_t<T>, std::experimental::filesystem::path>::value
#endif
        && !has_user_to_json<B, T>::value       // non-circular ADL probe
        && (is_reflectable_struct<false, T>::value || is_variant<T>::value);
};

template<typename B, typename T>
struct from_json_eligible
{
    static constexpr bool value =
        !in_json_namespace(^^T)                 // library-internal types (identity_tag, ...) never
        && std::is_class<T>::value              // participate — MUST precede the ADL probe
        && !std::is_union<T>::value
        && !std::is_scalar<T>::value
        && !std::is_array<T>::value
        && !nlohmann::detail::is_basic_json<T>::value
        && !is_string_like_from<B, T>::value    // from_json string overload probe
        && !nlohmann::detail::is_constructible_array_type<B, T>::value
        && !nlohmann::detail::is_constructible_object_type<B, T>::value
        && !nlohmann::detail::is_compatible_binary_type<B, T>::value
        && !nlohmann::detail::is_specialization_of<std::pair, T>::value
        && !nlohmann::detail::is_specialization_of<std::tuple, T>::value
#if JSON_HAS_FILESYSTEM
        && !std::is_same<std::remove_cvref_t<T>, std::filesystem::path>::value
#endif
#if JSON_HAS_EXPERIMENTAL_FILESYSTEM
        && !std::is_same<std::remove_cvref_t<T>, std::experimental::filesystem::path>::value
#endif
        && !is_optional<T>::value
        && !has_user_from_json<B, T>::value     // non-circular ADL probe
        && (is_reflectable_struct<false, T>::value || is_variant<T>::value);
};

// ---------------------------------------------------------------------------
// priority tag — overload-ranking dispatch (same idiom nlohmann uses
// internally); no if-constexpr-with-splices (see AGENTS.md §3).
// ---------------------------------------------------------------------------
template<std::size_t N> struct priority_tag : priority_tag < N - 1 > {};
template<> struct priority_tag<0> {};

// Eligibility for the adl branch. Containers (array-like, except strings;
// object-like) are handled by the refl2 array/object branches so plain
// reflected structs can be element/value types: nlohmann's from_json detection
// over-accepts object/array-like types whose element/value types are not
// really gettable (e.g. map<string, PlainStruct> — get<pair<const string,
// PlainStruct>> looks viable to the tuple machinery but breaks on
// instantiation). Element recursion is behavior-identical for everything
// nlohmann handles and additionally covers plain reflected structs.
//
// The array-like / C-array exclusions carve out the types nlohmann has
// DEDICATED overloads for (is_library_dedicated_array: binary_t, forward_list,
// valarray) and C arrays — those route through the adl branch (the library's
// own overloads) because the generic refl2 array branch would mis-handle them
// (binary_t as a number array, forward_list/valarray from_json). C arrays are
// no longer excluded (they now reach the library's C-array overloads).
//
// The trailing exclusion is the CIRCULARITY fix: a catch-all-eligible type
// whose adl_serializer is the primary template must NOT take the adl branch —
// adl_serializer<T, void>::to_json would route through the CPO into the
// catch-all and recurse at compile time. It is member-reflected instead. A
// user-specialized type (adl_serializer_is_primary false) DOES take the adl
// branch so the specialization wins.
template<typename B, typename T>
inline constexpr bool to_adl_branch_eligible_v =
    is_adl_serializable<B, T>::value
    && !is_object_like<T>::value
    && !(is_array_like<T>::value && !is_string_like_to<B, T>::value
         && !is_library_dedicated_array<B, T>)
    && !(to_json_eligible<B, T>::value && adl_serializer_is_primary<B, T>::value);

template<typename B, typename T>
inline constexpr bool from_adl_branch_eligible_v =
    is_adl_deserializable<B, T>::value
    && !is_object_like<T>::value
    && !(is_array_like<T>::value && !is_string_like_from<B, T>::value
         && !is_library_dedicated_array<B, T>)
    && !(from_json_eligible<B, T>::value && adl_serializer_is_primary<B, T>::value);

template<bool U, typename T>
inline constexpr std::size_t member_count_v = effective_member_count(^^T, access_context_for(U));

template<bool U, typename T, std::size_t I>
inline constexpr std::meta::info member_v = effective_member(^^T, access_context_for(U), I);

template<bool U, typename T, std::size_t I>
inline constexpr std::array<char, 64> member_key_v =
    member_json_key(effective_member(^^T, access_context_for(U), I));

// bit-field members cannot bind to a T& (no address), so from_json must
// assign via get<M>() instead of recursing into a T&
template<bool U, typename T, std::size_t I>
inline constexpr bool member_is_bit_field_v =
    std::meta::is_bit_field(member_v<U, T, I>);

// [[=refl2::json_default{}]] members: from_json falls back to T{}.member
// when the key is missing
template<bool U, typename T, std::size_t I>
inline constexpr bool member_has_default_v =
    has_annotation<json_default>(member_v<U, T, I>);

// ---------------------------------------------------------------------------
// Enum serialization (M6) — replaces NLOHMANN_JSON_SERIALIZE_ENUM(_STRICT).
// An enum with ANY enumerator annotated
//   [[=refl2::json_name{"..."}]] enumerator = value    (attribute after the
//   identifier — the only position GCC 16 accepts on enumerators) maps to
//   strings: json_name overrides the string, an unannotated enumerator falls
//   back to its identifier. An enum WITHOUT annotations keeps the library's
//   integer path byte-for-byte (zero drift). The macro stays as the C++11
//   path. Semantic notes vs the macro:
//     * to_json of an out-of-table value returns the first entry (macro
//       behavior);
//     * from_json accepts the mapped strings AND integers (macro: only table
//       entries) — documented divergence.
// ---------------------------------------------------------------------------
consteval std::size_t enum_count(std::meta::info enum_type)
{
    return std::meta::enumerators_of(enum_type).size();
}

// does the enum carry any json_name annotation on its enumerators?
template<typename E>
consteval bool enum_has_annotations()
{
    const std::size_t n = std::meta::enumerators_of(^^E).size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (has_annotation<json_name>(std::meta::enumerators_of(^^E)[i]))
        {
            return true;
        }
    }
    return false;
}

// compile-time enumerator facts (splice yields the enumerator VALUE; the
// strings reuse member_json_key — json_name first, identifier_of fallback)
template<typename E, std::size_t I>
inline constexpr std::underlying_type_t<E> enum_value_v =
    static_cast<std::underlying_type_t<E>>([: std::meta::enumerators_of(^^E)[I] :]);

template<typename E, std::size_t I>
inline constexpr std::array<char, 64> enum_string_v =
    member_json_key(std::meta::enumerators_of(^^E)[I]);

// runtime tables (index_sequence pack expansion — variable templates cannot
// be indexed by a runtime loop variable)
template<typename E, std::size_t... I>
inline std::vector<std::underlying_type_t<E>> enum_values_impl(std::index_sequence<I...>)
{
    return {enum_value_v<E, I>...};
}
template<typename E>
inline const std::vector<std::underlying_type_t<E>>& enum_values()
{
    static const std::vector<std::underlying_type_t<E>> table =
        enum_values_impl<E>(std::make_index_sequence<enum_count(^^E)> {});
    return table;
}

template<typename E, std::size_t... I>
inline std::vector<std::string> enum_strings_impl(std::index_sequence<I...>)
{
    return {std::string(enum_string_v<E, I>.data())...};
}
template<typename E>
inline const std::vector<std::string>& enum_strings()
{
    static const std::vector<std::string> table =
        enum_strings_impl<E>(std::make_index_sequence<enum_count(^^E)> {});
    return table;
}

// to_json: annotated enum -> string; unannotated -> the library's integer
// path verbatim (signedness-derived number type; self-contained so this
// header does not depend on templates defined AFTER it in {to,from}_json.hpp)
template<typename B, typename E>
void serialize_enum(B& j, E e)
{
    using U = std::underlying_type_t<E>;
    if constexpr (enum_has_annotations<E>())
    {
        const U v = static_cast<U>(e);
        const auto& values = enum_values<E>();
        const auto& strings = enum_strings<E>();
        std::size_t idx = 0; // macro to_json returns the first entry on miss
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (values[i] == v)
            {
                idx = i;
                break;
            }
        }
        j = strings[idx];
    }
    else if constexpr (std::is_unsigned<U>::value)
    {
        j = static_cast<typename B::number_unsigned_t>(e);
    }
    else
    {
        j = static_cast<typename B::number_integer_t>(e);
    }
}

// integer fallback mirroring detail::get_arithmetic_value (number branches
// only; self-contained for the same reason as serialize_enum)
template<typename B, typename U>
void enum_get_arithmetic(const B& j, U& val)
{
    if (j.is_number_unsigned())
    {
        val = static_cast<U>(*j.template get_ptr<const typename B::number_unsigned_t*>());
    }
    else if (j.is_number_integer())
    {
        val = static_cast<U>(*j.template get_ptr<const typename B::number_integer_t*>());
    }
    else if (j.is_number_float())
    {
        val = static_cast<U>(*j.template get_ptr<const typename B::number_float_t*>());
    }
    else
    {
        JSON_THROW(nlohmann::detail::type_error::create(
                       302, nlohmann::detail::concat("type must be number, but is ", j.type_name()), &j));
    }
}

// from_json: annotated enum accepts the mapped strings (json_name /
// identifier) and falls back to the integer path for numbers; unannotated
// enums keep the integer path verbatim.
template<typename B, typename E>
void deserialize_enum(const B& j, E& e)
{
    using U = std::underlying_type_t<E>;
    if constexpr (enum_has_annotations<E>())
    {
        if (j.is_string())
        {
            const auto& s = j.template get_ref<const typename B::string_t&>();
            const auto& strings = enum_strings<E>();
            const auto& values = enum_values<E>();
            for (std::size_t i = 0; i < strings.size(); ++i)
            {
                if (strings[i] == s)
                {
                    e = static_cast<E>(values[i]);
                    return;
                }
            }
            JSON_THROW(nlohmann::detail::type_error::create(
                           302, nlohmann::detail::concat("cannot parse enum string '", s, "'"), &j));
        }
        // non-string JSON: integer path (mirrors the macro accepting integer
        // mappings; documented divergence)
    }
    U val{};
    enum_get_arithmetic(j, val);
    e = static_cast<E>(val);
}

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
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<7>)
    {
        j = v; // native value nesting; MUST precede adl (string_like trap)
    }

    template<typename B, typename T>
    requires detail::is_variant<T>::value
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<6>)
    {
        // oneof wire format: {"index": <active alternative index>,
        // "value": <active alternative serialized>}; std::monostate carries
        // no payload and serializes as null
        j = B::object();
        j["index"] = v.index();
        std::visit([&](const auto& alt)
        {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(alt)>, std::monostate>)
            {
                j["value"] = nullptr;
            }
            else
            {
                serialize_one(j["value"], alt);
            }
        }, v);
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
    requires detail::to_adl_branch_eligible_v<B, T>
    static void serialize_one_impl(B& j, const T& v, detail::priority_tag<4>)
    {
        nlohmann::adl_serializer<T, void>::to_json(j, v);
    }

    template<typename B, typename T>
    requires (detail::is_array_like<T>::value && !detail::is_library_dedicated_array<B, T>)
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
                      "refl2: duplicate JSON keys across the class hierarchy "
                      "(member names or json_name annotations colliding) would "
                      "overwrite in the JSON object");
        reflect_to_json(j, v);
    }

    template<typename B, typename T>
    static void serialize_one_impl(B&, const T&, detail::priority_tag<0>)
    {
        static_assert(always_false<T>::value,
                      "refl2: not serializable — no adl_serializer/to_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a to_json for the "
                      "type (or specialize nlohmann::adl_serializer). "
                      "Unions are not supported, nor are "
                      "pointers/self-referential types.");
    }

    template<typename B, typename T>
    static void serialize_one(B& j, const T& v)
    {
        serialize_one_impl(j, v, detail::priority_tag<7> {});
    }

    // ---- from_json side (symmetric) --------------------------------------
    template<typename B, typename T>
    requires detail::is_nested_json<B, T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<7>)
    {
        v = j;
    }

    template<typename B, typename T>
    requires detail::is_variant<T>::value
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<6>)
    {
        // runtime index dispatch over the alternatives (index_sequence fold);
        // each alternative must be default-constructible (emplace<I>())
        const std::size_t idx = j.at("index").template get<std::size_t>();
        deserialize_variant_impl<B, T>(j, v, idx, std::make_index_sequence<std::variant_size_v<T>> {});
    }

    template<typename B, typename T, std::size_t... I>
    static void deserialize_variant_impl(const B& j, T& v, const std::size_t idx,
                                         std::index_sequence<I...>)
    {
        bool handled = false;
        (..., (idx == I ? (handled = true, emplace_and_deserialize<B, T, I>(j, v)) : void()));
        if (!handled)
        {
            JSON_THROW(nlohmann::detail::type_error::create(
                           302, nlohmann::detail::concat("cannot parse variant: index ",
                                                         std::to_string(idx), " out of range (0..",
                                                         std::to_string(std::variant_size_v<T> - 1), ")"),
                           &j));
        }
    }

    template<typename B, typename T, std::size_t I>
    static void emplace_and_deserialize(const B& j, T& v)
    {
        using Alt = std::variant_alternative_t<I, T>;
        v.template emplace<I>();
        if constexpr (!std::is_same_v<Alt, std::monostate>)
        {
            deserialize_one(j.at("value"), std::get<I>(v));
        }
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
    requires detail::from_adl_branch_eligible_v<B, T>
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<4>)
    {
        nlohmann::adl_serializer<T, void>::from_json(j, v);
    }

    template<typename B, typename T>
    requires (detail::is_array_like<T>::value && !detail::is_library_dedicated_array<B, T>)
    static void deserialize_one_impl(const B& j, T& v, detail::priority_tag<3>)
    {
        if constexpr (requires { v.push_back(typename T::value_type{}); })
        {
            // sequential containers (vector, list, deque): append
            v.clear();
            for (auto&& e : j)
            {
                typename T::value_type elem{};
                deserialize_one(e, elem);
                v.push_back(std::move(elem));
            }
        }
        else if constexpr (requires { v.insert(typename T::value_type{}); })
        {
            // insert-based associative containers (set, multiset,
            // unordered_set): no push_back AND no operator[] — must not fall
            // into the fixed-size index-assign branch below (set has no
            // operator[]; that branch used to be a hard compile error).
            v.clear();
            for (auto&& e : j)
            {
                typename T::value_type elem{};
                deserialize_one(e, elem);
                v.insert(std::move(elem));
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
                      "refl2: duplicate JSON keys across the class hierarchy "
                      "(member names or json_name annotations colliding) would "
                      "overwrite in the JSON object");
        reflect_from_json(j, v);
    }

    template<typename B, typename T>
    static void deserialize_one_impl(const B&, T&, detail::priority_tag<0>)
    {
        static_assert(always_false<T>::value,
                      "refl2: not deserializable — no adl_serializer/from_json "
                      "customization, not a reflectable struct, and not an "
                      "array/object-like container. Define a from_json for the "
                      "type (or specialize nlohmann::adl_serializer). "
                      "Unions are not supported, nor are "
                      "pointers/self-referential types, nor "
                      "non-default-constructible types (the T& form needs "
                      "an existing object).");
    }

    template<typename B, typename T>
    static void deserialize_one(const B& j, T& v)
    {
        deserialize_one_impl(j, v, detail::priority_tag<7> {});
    }

    // ---- reflected struct: member loop with pre-built static keys ---------
    template<typename B, typename T, std::size_t I>
    static void serialize_member(B& j, const T& v)
    {
        // one-time-initialized key (any length; thread-safe magic static);
        // no per-call std::string construction and no per-member constructor
        // call site in the hot path
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>.data());
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
        reflect_to_json_impl(j, v, std::make_index_sequence<detail::member_count_v<Unchecked, T>> {});
    }

    // bit-fields cannot bind to a T& (no address): tag-dispatch instead of
    // if-constexpr-with-splices (GCC 16 does not discard the false branch);
    // explicit <B,T,I> needed — I is not deducible from the tag argument.
    // json_default members fall back to T{}.member when the key is missing.
    template<typename B, typename T, std::size_t I>
    static void deserialize_member(const B& j, T& v)
    {
        deserialize_member_impl<B, T, I>(
            j, v,
            std::bool_constant<detail::member_has_default_v<Unchecked, T, I>> {},
            std::bool_constant<detail::member_is_bit_field_v<Unchecked, T, I>> {});
    }

    // (json_default, bit-field)
    template<typename B, typename T, std::size_t I>
    static void deserialize_member_impl(const B& j, T& v, std::true_type, std::true_type)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>.data());
        using M = typename [: std::meta::type_of(detail::member_v<Unchecked, T, I>) :];
        const auto it = j.find(key);
        if (it != j.end())
        {
            v.[:detail::member_v<Unchecked, T, I>:] = it->template get<M>();
        }
        else
        {
            v.[:detail::member_v<Unchecked, T, I>:] = T{}. [:detail::member_v<Unchecked, T, I>:];
        }
    }

    // (json_default, ordinary)
    template<typename B, typename T, std::size_t I>
    static void deserialize_member_impl(const B& j, T& v, std::true_type, std::false_type)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>.data());
        const auto it = j.find(key);
        if (it != j.end())
        {
            deserialize_one(*it, v.[:detail::member_v<Unchecked, T, I>:]);
        }
        else
        {
            v.[:detail::member_v<Unchecked, T, I>:] = T{}. [:detail::member_v<Unchecked, T, I>:];
        }
    }

    // (no default, bit-field)
    template<typename B, typename T, std::size_t I>
    static void deserialize_member_impl(const B& j, T& v, std::false_type, std::true_type)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>.data());
        using M = typename [: std::meta::type_of(detail::member_v<Unchecked, T, I>) :];
        v.[:detail::member_v<Unchecked, T, I>:] = j.at(key).template get<M>();
    }

    // (no default, ordinary)
    template<typename B, typename T, std::size_t I>
    static void deserialize_member_impl(const B& j, T& v, std::false_type, std::false_type)
    {
        static const std::string key = std::string(detail::member_key_v<Unchecked, T, I>.data());
        deserialize_one(j.at(key), v.[:detail::member_v<Unchecked, T, I>:]);
    }

    template<typename B, typename T, std::size_t... I>
    static void reflect_from_json_impl(const B& j, T& v, std::index_sequence<I...>)
    {
        (..., deserialize_member<B, T, I>(j, v));
    }

    template<typename B, typename T>
    static void reflect_from_json(const B& j, T& v)
    {
        reflect_from_json_impl(j, v, std::make_index_sequence<detail::member_count_v<Unchecked, T>> {});
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

// ---------------------------------------------------------------------------
// The catch-all overloads. Declared in nlohmann::detail BEFORE to_json_fn /
// from_json_fn (the gated includes in {to,from}_json.hpp sit at the top of
// those files), so the unqualified call inside the CPO's operator() finds
// them by ordinary lookup. The requires-clause is the negative mirror of the
// existing overload sets — for every type the library already handles, one
// of the existing (more specialized) overloads is selected instead, and for
// every user-customized type the user's to_json/from_json wins.
// ---------------------------------------------------------------------------
NLOHMANN_JSON_NAMESPACE_BEGIN
namespace detail
{

template<typename BasicJsonType, typename T>
requires refl2::detail::to_json_eligible<BasicJsonType, T>::value
inline void to_json(BasicJsonType& j, const T& v)
{
    refl2::codec<false>::to_json(j, v);
}

template<typename BasicJsonType, typename T>
requires refl2::detail::from_json_eligible<BasicJsonType, T>::value
inline void from_json(const BasicJsonType& j, T& v)
{
    refl2::codec<false>::from_json(j, v);
}

} // namespace detail
NLOHMANN_JSON_NAMESPACE_END
