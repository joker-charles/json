// concepts.hpp — C++20 concepts layer for nlohmann::detail (modernization).
//
// Modernization track of the feature/static-reflection branch: replace the
// hand-written SFINAE detection traits with C++20 concepts where the semantics
// are a pure "category check" on a template parameter. This header is gated by
// JSON_HAS_CPP_20 so the library keeps a complete C++11 path.
//
// Design rules (see docs/static-reflection/M4_ASSESSMENT.md):
//   * Only express "is this type in library category X" — the C-class of pure
//     external-type-capability probes (has_to_json / is_compatible_*_type
//     against user types that may not exist yet) stays in type_traits.hpp and
//     is deliberately NOT concept-ified: it needs SFINAE soft-failure, which a
//     concept cannot provide.
//   * A concept constrained overload must select the SAME overload as the
//     enable_if_t it replaces (verified by differential tests).
//   * Consumers in to_json.hpp / from_json.hpp use `#ifdef JSON_HAS_CPP_20`
//     to route to the concept-constrained overloads, else the enable_if ones.

#pragma once

#include <type_traits> // is_integral, is_floating_point, is_enum, is_same, underlying_type
#include <utility>     // declval

#include <nlohmann/detail/macro_scope.hpp> // JSON_HAS_CPP_20, JSON_HAS_RANGES

#ifdef JSON_HAS_CPP_20

// Dependency note: this header does NOT include type_traits.hpp (doing so
// would re-define make_void/nonesuch/detector in TUs that already bring in the
// full library). Consumers that instantiate the layer-2 concepts must include
// <nlohmann/detail/meta/type_traits.hpp> first (nlohmann's conversions headers
// already do). The layer-2 concepts reference nlohmann::detail::is_constructible
// which has pair/tuple specializations the concepts must match (see §7).

namespace nlohmann
{
namespace detail
{
namespace concepts
{

// ---- arithmetic scalar categories (map 1:1 to enable_if_t<is_*>) ----
template<typename T>
concept integral_not_bool =
    std::is_integral_v<std::remove_cvref_t<T>> &&
    !std::is_same_v<std::remove_cvref_t<T>, bool>;

template<typename T>
concept boolean_like =
    std::is_same_v<std::remove_cvref_t<T>, bool>;

template<typename T>
concept floating_point =
    std::is_floating_point_v<std::remove_cvref_t<T>>;

template<typename T>
concept enum_type =
    std::is_enum_v<std::remove_cvref_t<T>>;

// ---------------------------------------------------------------------------
// Layer 1: atomic probes (replace the is_detected templates).
// NOTE: each probe must mirror the EXACT target the corresponding library trait
// checks — `iterator` (not value_type), `mapped_type`+`key_type`, etc. See
// tests/static-reflection/probe_layered_concepts.cpp (zero-drift benchmark).
// ---------------------------------------------------------------------------
template<typename T>
concept has_iterator_type =
    requires { typename std::remove_cvref_t<T>::iterator; };

template<typename T>
concept has_iterator_traits =
    has_iterator_type<T> &&
    requires {
        typename std::remove_cvref_t<T>::iterator::value_type;
        typename std::remove_cvref_t<T>::iterator::difference_type;
    };

template<typename T>
concept has_mapped_and_key =
    requires { typename std::remove_cvref_t<T>::mapped_type;
               typename std::remove_cvref_t<T>::key_type; };

// the element type reachable through the container's iterator
template<typename T>
using iter_value_t = typename std::remove_cvref_t<T>::iterator::value_type;

// ---------------------------------------------------------------------------
// Layer 2: reusable semantic concepts. Each carries BasicJsonType because
// array/object/string compatibility is "element/key/value can construct B".
// These intentionally EXCLUDE the range-view dimension (see note at bottom).
// `nlohmann::detail::is_constructible` must be declared (via type_traits.hpp).
// ---------------------------------------------------------------------------

// string_like<B, T> == is_compatible_string_type<B, T>
template<typename B, typename T>
concept string_like =
    nlohmann::detail::is_constructible<typename B::string_t, T>::value;

// object_like<B, T> — mirrors is_compatible_object_type<B, T>
template<typename B, typename T>
concept object_like =
    has_mapped_and_key<T> &&
    nlohmann::detail::is_constructible<typename B::object_t::key_type,
                                       typename T::key_type>::value &&
    nlohmann::detail::is_constructible<typename B::object_t::mapped_type,
                                       typename T::mapped_type>::value;

// array_like<B, T> — mirrors is_compatible_array_type<B, T> (iterator/construct)
// The filesystem::path special case (T != its own range_value) is preserved.
template<typename B, typename T>
concept array_like =
    has_iterator_traits<T> &&
    !std::is_same_v<std::remove_cvref_t<T>, iter_value_t<T>> &&
    nlohmann::detail::is_constructible<B, iter_value_t<T>>::value;

// NOTE on the range-view dimension: is_compatible_range_view<T> deliberately
// stays a trait (referenced in layer-3 requires, not folded into array_like).
// It wraps SafeToCheck guards (is_iteration_proxy_type / is_basic_json /
// is_range_view_optional_type) to avoid std::ranges circular-constraint
// crashes on GCC; restating that logic inside a concept is high-risk.

} // namespace concepts
} // namespace detail
} // namespace nlohmann

#endif // JSON_HAS_CPP_20
