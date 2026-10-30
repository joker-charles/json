//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (reflection extension)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Standalone flat-struct serialization add-on (<nlohmann/reflection.hpp>).
//
// This is an EXPLICIT ADD-ON: <nlohmann/json.hpp> does not include it, and the
// amalgamated single header does not ship it. Opt in with the feature macro on
// top of the explicit include:
//
//     #define JSON_USE_REFLECTION
//     #include <nlohmann/reflection.hpp>
//
//     struct [[=nlohmann::json_serializable{}]] point
//     {
//         double x{};
//         double y{};
//     };
//
//     nlohmann::json j = point{1.0, 2.0}; // {"x":1.0,"y":2.0}
//     auto p = j.get<point>();
//
// Scope:
//   * opt-in by type: only classes/unions annotated with
//     [[=nlohmann::json_serializable{}]] participate; every other type keeps
//     the library's existing behavior byte-for-byte;
//   * flat structs only: public non-static data members; no base classes,
//     no unions, no private/protected data members, no anonymous members;
//   * nested member types use the normal serialization path (library types,
//     ADL to_json/from_json, or another annotated type) -- this header does
//     not recursively reflect unannotated nested structs;
//   * no member rename/skip/default annotations, no enum/variant reflection,
//     no binary formats. Those are later slices.
//
// Public API only: the serializer is a SFINAE-constrained partial
// specialization of nlohmann::adl_serializer<T, void> (no C++20 concepts
// layer is involved) and builds/reads values through
// BasicJsonType::object(), operator[], at() and get<T>(). It never touches
// basic_json's private json_value/m_data layout or reflection's unchecked().
//
// Opt-in and gating:
//   * without JSON_USE_REFLECTION this header is a no-op, so an accidental
//     include changes nothing and the C++11 per-header self-containment check
//     stays green;
//   * with JSON_USE_REFLECTION a C++26 P2996 toolchain is required (g++-16
//     -std=c++26 -freflection); on any other toolchain the header fails with a
//     clear #error rather than a silent no-op that would surface later as
//     "json_serializable not declared" at the annotation.
//
// JSON_USE_REFLECTION is consulted only by this header; no core header reads
// it. It is a presence switch: any definition enables the extension, so define
// it in exactly one place (the source file or the compiler command line) before
// the first include of this header. Do not mix an annotated type with a
// hand-written serializer for the same type.

#pragma once

#if defined(JSON_USE_REFLECTION)

#include <nlohmann/json.hpp>

#if !defined(__cpp_impl_reflection) || !defined(__cpp_lib_reflection)
    #error "JSON_USE_REFLECTION requires C++26 static reflection (P2996): use GCC 16 and -std=c++26 -freflection"
#else

#include <meta>
#include <string>
#include <type_traits>
#include <utility>

NLOHMANN_JSON_NAMESPACE_BEGIN

struct json_serializable {};

namespace detail
{
namespace reflection_flat
{

consteval bool has_json_serializable_annotation(std::meta::info type)
{
    for (const auto annotation : std::meta::annotations_of(type))
    {
        if (std::meta::is_same_type(
                    std::meta::remove_cvref(std::meta::type_of(annotation)),
                    ^^json_serializable))
        {
            return true;
        }
    }
    return false;
}

template<typename T, typename = void>
struct is_complete : std::false_type {};

template<typename T>
struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

template<typename T>
inline constexpr bool is_annotated_v =
    !std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_reference_v<T>
    && (std::is_class_v<T> || std::is_union_v<T>)
    && is_complete<T>::value
    && has_json_serializable_annotation(^^T);

template<typename>
inline constexpr bool dependent_false = false;

template<typename T>
constexpr void validate_flat_struct()
{
    static_assert(!std::meta::is_union_type(^^T),
                  "json_serializable does not support unions");
    static_assert(std::meta::bases_of(^^T, std::meta::access_context::unprivileged()).empty()
                  && !std::meta::has_inaccessible_bases(^^T, std::meta::access_context::unprivileged()),
                  "json_serializable flat-struct serializer does not support base classes");
    static_assert(!std::meta::has_inaccessible_nonstatic_data_members(
                      ^^T, std::meta::access_context::unprivileged()),
                  "json_serializable requires all non-static data members to be public");
}

// SFINAE probes: can this member be written/read through the public interface?
// (No C++20 concepts: a well-formed expression in a void_t argument makes the
// trait true, substitution failure makes it false.)
template<typename BasicJsonType, typename T, std::meta::info member, typename = void>
struct member_to_json : std::false_type {};

template<typename BasicJsonType, typename T, std::meta::info member>
struct member_to_json<BasicJsonType, T, member,
    std::void_t<decltype(std::declval<BasicJsonType&>()[std::declval<const std::string&>()]
                         = std::declval<const T&>().[:member:])>> : std::true_type {};

template<typename BasicJsonType, typename T, std::meta::info member, typename = void>
struct member_from_json : std::false_type {};

template<typename BasicJsonType, typename T, std::meta::info member>
struct member_from_json<BasicJsonType, T, member,
    std::void_t<decltype(std::declval<T&>().[:member:]
                         = std::declval<const BasicJsonType&>()
                               .at(std::declval<const std::string&>())
                               .template get<typename [:std::meta::type_of(member):]>())>>
    : std::true_type {};

} // namespace reflection_flat
} // namespace detail

// SFINAE-constrained partial specialization: for annotated types the second
// argument resolves to void, the default of the public adl_serializer
// customization point, so no C++20 concept is needed.
template<typename T>
struct adl_serializer<T, std::enable_if_t<detail::reflection_flat::is_annotated_v<T>>>
{
    template<typename BasicJsonType>
    static void to_json(BasicJsonType& j, const T& value)
    {
        detail::reflection_flat::validate_flat_struct<T>();

        j = BasicJsonType::object();
        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(
                              ^^T, std::meta::access_context::unprivileged())))
        {
            using member_type = typename [:std::meta::type_of(member):];

            if constexpr (std::meta::has_identifier(member))
            {
                const auto key = std::string(std::meta::identifier_of(member));
                if constexpr (detail::reflection_flat::member_to_json<BasicJsonType, T, member>::value)
                {
                    j[key] = value.[:member:];
                }
                else
                {
                    static_assert(
                        detail::reflection_flat::dependent_false<member_type>,
                        "json_serializable member is not JSON-serializable; "
                        "provide ADL to_json() or annotate the member type");
                }
            }
            else
            {
                static_assert(
                    detail::reflection_flat::dependent_false<member_type>,
                    "json_serializable does not support anonymous members");
            }
        }
    }

    template<typename BasicJsonType>
    static void from_json(const BasicJsonType& j, T& value)
    {
        detail::reflection_flat::validate_flat_struct<T>();

        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(
                              ^^T, std::meta::access_context::unprivileged())))
        {
            using member_type = typename [:std::meta::type_of(member):];

            if constexpr (std::meta::has_identifier(member))
            {
                const auto key = std::string(std::meta::identifier_of(member));
                if constexpr (detail::reflection_flat::member_from_json<BasicJsonType, T, member>::value)
                {
                    value.[:member:] = j.at(key).template get<member_type>();
                }
                else
                {
                    static_assert(
                        detail::reflection_flat::dependent_false<member_type>,
                        "json_serializable member is not JSON-deserializable; "
                        "provide ADL from_json() or annotate the member type");
                }
            }
            else
            {
                static_assert(
                    detail::reflection_flat::dependent_false<member_type>,
                    "json_serializable does not support anonymous members");
            }
        }
    }
};

NLOHMANN_JSON_NAMESPACE_END

#endif // P2996 toolchain

#endif // JSON_USE_REFLECTION
