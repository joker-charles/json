//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (reflection extension)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Standalone flat-struct serialization extension (feature/static-reflection
// branch, first slice).
//
// This is an EXPLICIT ADD-ON: <nlohmann/json.hpp> does not include it, and the
// amalgamated single header does not ship it. Include it yourself:
//
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
// Scope of this first slice:
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
// Public API only: the serializer is a constrained partial specialization of
// nlohmann::adl_serializer<T, void> and builds/reads values through
// BasicJsonType::object(), operator[], at() and get<T>(). It never touches
// basic_json's private json_value/m_data layout or reflection's unchecked().
//
// Build requirement: C++26 static reflection (g++-16 -std=c++26
// -freflection). On a toolchain without P2996 this header is inert: including
// it compiles, but nlohmann::json_serializable is not defined and annotated
// code fails at the annotation. This keeps the C++11 per-header
// self-containment check green.
//
// The legacy JSON_USE_REFLECTION opt-in (reflection_to_json.hpp's wide-open
// catch-all) is a separate experiment. This header neither requires nor
// defines it; do not mix an annotated type with a hand-written/legacy
// serializer for the same type.

#pragma once

#include <nlohmann/json.hpp>

#if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)

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

template<typename T>
inline constexpr bool is_annotated_v =
    !std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_reference_v<T>
    && (std::is_class_v<T> || std::is_union_v<T>)
    && requires { sizeof(T); }
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

template<typename BasicJsonType, typename T, std::meta::info member>
concept member_to_json = requires(BasicJsonType& out, const T& in)
{
    out[std::declval<const std::string&>()] = in.[:member:];
};

template<typename BasicJsonType, typename T, std::meta::info member>
concept member_from_json = requires(const BasicJsonType& in, T& out)
{
    out.[:member:] = in.at(std::declval<const std::string&>()).template get<typename [:std::meta::type_of(member):]>();
};

} // namespace reflection_flat
} // namespace detail

template<typename T>
requires detail::reflection_flat::is_annotated_v<T>
struct adl_serializer<T, void>
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
                if constexpr (detail::reflection_flat::member_to_json<BasicJsonType, T, member>)
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
                if constexpr (detail::reflection_flat::member_from_json<BasicJsonType, T, member>)
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

#endif
