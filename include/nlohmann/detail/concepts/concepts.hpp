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
#include <iterator>    // std::begin, std::end (for has_begin_end range probe)
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

// NOTE: integral_not_bool has no safe use point and is deliberately NOT wired
// into any overload (see M4_ASSESSMENT §5): replacing
// is_compatible_integer_type<B::number_*, T> with a pure is_integral would
// over-accept mismatched-signed / non-constructible integers (semantic drift).

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

// A type usable as an array source must be a RANGE: begin(t)/end(t) must be
// callable (mirrors the library's is_range -> result_of_begin/result_of_end).
// This is what excludes json::reverse_iterator and other non-range iterators
// from array_like (drift fix, see M4_ASSESSMENT §7).
template<typename T>
concept has_begin_end =
    requires(std::remove_cvref_t<T>& t)
{
    std::begin(t);
    std::end(t);
};

template<typename T>
concept has_mapped_and_key =
    requires { typename std::remove_cvref_t<T>::mapped_type;
               typename std::remove_cvref_t<T>::key_type;
             };

// ---------------------------------------------------------------------------
// Layer 2: reusable semantic concepts. Each carries BasicJsonType because
// array/object/string compatibility is "element/key/value can construct B".
// These intentionally EXCLUDE the range-view dimension (see note at bottom).
// `nlohmann::detail::is_constructible` must be declared (via type_traits.hpp).
//
// array_like MUST route its iterator through the library's begin-based
// iterator_t/range_value_t (not T::iterator): views (e.g.
// std::ranges::reverse_view<ref_view<json>>) have begin() but no `iterator`
// alias, so a `T::iterator` probe rejects them while the library's
// is_compatible_array_type accepts them. See M4_ASSESSMENT §7 drift case.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PARAMETER ORDER — a hard GCC rule, do not reorder these back to <B, T>.
//
// These are *partial-application* concepts: they take the candidate type `T`
// in-front of the `BasicJsonType` `B`. The reason is that GCC resolves a
// constrained placeholder `concepts::X<BasicJsonType> Name` (to_json string
// overload) by binding `Name` to the FIRST template parameter and the explicit
// `<BasicJsonType>` argument to the SECOND. If the concept were declared
// `<B, T>` (BasicJsonType in front) that would yield
// `X<Name = B, BasicJsonType = T>` — i.e. the wrong type winds up in `B`, and
// `typename B::string_t` then fails on a user type like a custom `alt_string`
// string_t. Declaring the candidate first (`<T, B>`) aligns both the
// constrained-placeholder form and the explicit `requires(X<T, B> ...)` form
// in to_json.hpp. Verified minimal repro (g++-16, -std=c++20).
// ---------------------------------------------------------------------------

// boolean_like<T, B> — mirrors the to_json boolean overload's
// `is_same<T, B::boolean_t>` exactly. Takes B because boolean_t is a library
// type parameter (defaults to bool but is customizable), NOT hard-coded bool.
template<typename T, typename B>
concept boolean_like =
    std::is_same_v<T, typename B::boolean_t>;

// string_like<T, B> == is_compatible_string_type<B, T>
template<typename T, typename B>
concept string_like =
    nlohmann::detail::is_constructible<typename B::string_t, T>::value;

// object_like<T, B> — mirrors is_compatible_object_type<B, T>
template<typename T, typename B>
concept object_like =
    has_mapped_and_key<T> &&
    nlohmann::detail::is_constructible<typename B::object_t::key_type,
    typename T::key_type>::value &&
    nlohmann::detail::is_constructible<typename B::object_t::mapped_type,
    typename T::mapped_type>::value;

// array_like<T, B> — mirrors is_compatible_array_type<B, T>.
// Uses the library's is_range (begin/end + iterator-traits) and range_value_t
// so that range views are accepted exactly as the baseline trait does, keeping
// byte-identical overload selection.
template<typename T, typename B>
concept array_like =
    has_begin_end<T> &&
    nlohmann::detail::is_range<T>::value &&
    !std::is_same_v<std::remove_cvref_t<T>,
    nlohmann::detail::range_value_t<T>> &&
    nlohmann::detail::is_constructible<B,
    nlohmann::detail::range_value_t<T>>::value;

// NOTE on the range-view dimension: is_compatible_range_view<T> deliberately
// stays a trait (referenced in layer-3 requires, not folded into array_like).
// It wraps SafeToCheck guards (is_iteration_proxy_type / is_basic_json /
// is_range_view_optional_type) to avoid std::ranges circular-constraint
// crashes on GCC; restating that logic inside a concept is high-risk.

} // namespace concepts
} // namespace detail
} // namespace nlohmann

#endif // JSON_HAS_CPP_20
