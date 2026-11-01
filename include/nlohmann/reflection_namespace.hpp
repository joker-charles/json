//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (reflection extension)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://github.com/nlohmann/json>
// SPDX-License-Identifier: MIT
//
// Reflection-namespace opt-in add-on (<nlohmann/reflection_namespace.hpp>).
//
// This header is an EXPERIMENT on top of <nlohmann/reflection.hpp>: it keeps
// the same public-API-only serialization core and adds two further ways to
// opt a type in, both requested in discussion #5384.
//
//   * per-type `nlohmann::reflection::enable<T>`:
//
//         namespace third_party { struct widget { int id; }; }
//         namespace nlohmann::reflection
//         {
//             template<> inline constexpr bool enable<third_party::widget> = true;
//         }
//
//     This works for types you do not own and needs no `using` declaration:
//     a constrained adl_serializer specialization forwards to the plain
//     functions below. It also makes `[[=json_serializable{}]]` optional sugar
//     rather than the only entry point.
//
//   * namespace-wide `using`:
//
//         namespace dm
//         {
//             using nlohmann::reflection::to_json;
//             using nlohmann::reflection::from_json;
//         }
//
//     The functions are ordinary templates; the using-declaration makes them
//     visible to ADL for every type in `dm`. In the default (constrained) mode
//     `using` alone does NOT opt a type in: it only makes the plain functions
//     findable for types that are already enabled/annotated. This preserves the
//     library's behavior for unannotated types and avoids the maintainer's
//     "namespace-wide using sends every type through the reflective path"
//     caveat.
//
//   * `JSON_REFLECTION_NAMESPACE_UNCONSTRAINED` (opt-in macro, for the
//     experiment and for measuring the caveat): the plain functions lose the
//     is_reflectable constraint, so `using` alone opts in every class/union in
//     the namespace. Unsupported types then fail inside the serializer body
//     instead of keeping today's "no serialization path" error. This is the
//     behavior Anketell described as "the safety would be off"; do not enable
//     it in a header you do not control.
//
// Precedence (verified by tests/static-reflection/optin_conflicts.cpp):
//   * an explicit adl_serializer<T> specialization wins over everything here;
//   * an annotation or enable<T> selects the forwarding adl_serializer
//     specialization, which bypasses a hand-written ADL to_json/from_json;
//   * namespace `using` goes through ADL, so a non-template hand-written
//     to_json/from_json wins over the reflection template.
//
// Gating: `JSON_USE_REFLECTION` is still required, and a P2996 toolchain is
// still required; without the macro this header is a no-op. The header is not
// amalgamated into single_include and is not included by json.hpp.

#pragma once

#if defined(JSON_USE_REFLECTION)

#include <nlohmann/reflection.hpp>

// reflection.hpp already emits the single actionable #error when the macro is
// defined without P2996; do not repeat it here.
#if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)

#include <type_traits>

NLOHMANN_JSON_NAMESPACE_BEGIN

namespace reflection
{

/// Per-type opt-in customization point. Specialize it for a type you own or a
/// type you do not own; no `using` declaration is needed for this route.
template<typename T>
inline constexpr bool enable = false;

/// True when T is a complete class/union that opted in either through the
/// annotation or through `enable<T>`. Public so callers can dispatch on it.
template<typename T>
inline constexpr bool is_reflectable_v =
    (std::is_class_v<T> || std::is_union_v<T>)
    && ::nlohmann::detail::reflection_flat::is_complete<T>::value
    && (::nlohmann::detail::reflection_flat::is_annotated_v<T> || enable<T>);

namespace detail
{

/// True only for the enable<T> route, so the forwarding adl_serializer
/// specialization below never competes with the annotation specialization in
/// <nlohmann/reflection.hpp> for the same T.
template<typename T>
inline constexpr bool is_enabled_only_v =
    is_reflectable_v<T> && enable<T>
    && !::nlohmann::detail::reflection_flat::is_annotated_v<T>;

} // namespace detail

#if defined(JSON_REFLECTION_NAMESPACE_UNCONSTRAINED)
/// Unconstrained experimental mode: `using` alone opts in every class/union.
template<typename BasicJsonType, typename T,
         std::enable_if_t<(std::is_class_v<T> || std::is_union_v<T>)
                          && ::nlohmann::detail::reflection_flat::is_complete<T>::value, int> = 0>
#else
/// Default (constrained) mode: only enabled/annotated types are candidates.
template<typename BasicJsonType, typename T,
         std::enable_if_t<is_reflectable_v<T>, int> = 0>
#endif
void to_json(BasicJsonType& j, const T& value)
{
    ::nlohmann::detail::reflection_flat::validate_flat_struct<T>();
    ::nlohmann::detail::reflection_flat::to_object(j, value);
}

#if defined(JSON_REFLECTION_NAMESPACE_UNCONSTRAINED)
template<typename BasicJsonType, typename T,
         std::enable_if_t<(std::is_class_v<T> || std::is_union_v<T>)
                          && ::nlohmann::detail::reflection_flat::is_complete<T>::value, int> = 0>
#else
template<typename BasicJsonType, typename T,
         std::enable_if_t<is_reflectable_v<T>, int> = 0>
#endif
void from_json(const BasicJsonType& j, T& value)
{
    ::nlohmann::detail::reflection_flat::validate_flat_struct<T>();
    ::nlohmann::detail::reflection_flat::from_object(j, value);
}

} // namespace reflection

// Anketell's per-type route: selecting this adl_serializer specialization for
// enable<T> means a single specialization is enough -- no `using` is needed,
// and a third-party type can be enabled without touching its definition or
// reopening its namespace. It bypasses ADL, so a hand-written ADL
// to_json/from_json for the same type is not consulted; see the precedence
// note at the top of this file.
template<typename T>
struct adl_serializer<T, std::enable_if_t<reflection::detail::is_enabled_only_v<T>>>
{
    template<typename BasicJsonType>
    static void to_json(BasicJsonType& j, const T& value)
    {
        reflection::to_json(j, value);
    }

    template<typename BasicJsonType>
    static void from_json(const BasicJsonType& j, T& value)
    {
        reflection::from_json(j, value);
    }
};

NLOHMANN_JSON_NAMESPACE_END

#endif // P2996 toolchain

#endif // JSON_USE_REFLECTION
