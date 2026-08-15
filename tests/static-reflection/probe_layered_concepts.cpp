// probe_layered_concepts.cpp — verify the 3-layer concepts strategy (layer1 atomic
// probes + layer2 semantic concepts WITH BasicJsonType param + precise probe
// targets) equals the real library traits over a type matrix (zero drift).
// Build: g++-16 -std=c++26 -freflection -O0 -Isingle_include -Iinclude
// Layered concepts probe: implement the 3-layer strategy but with PRECISE
// semantics matching the real traits (BasicJsonType parameter, exact probe
// targets). Verify zero-drift against the library traits over a type matrix.
#include <cstdio>
#include <type_traits>
#include <vector>
#include <map>
#include <string>
#include <set>
#include <cstdint>
#include <typeinfo>
#include <cstring>

#include <nlohmann/json.hpp>
using json = nlohmann::json;
using B = json;

// ===== layer 1: atomic probes (replacing is_detected) =====
// NOTE: must mirror the EXACT probe targets of the real traits:
//   array  -> T::iterator (not value_type!), with complete iterator_traits
//   object -> T::mapped_type AND T::key_type
//   string -> constructible<string_t, T>  (constructibility, not convertibility)
template<typename T>
concept has_iterator_type = requires { typename T::iterator; };

template<typename T>
concept has_iterator_traits =
    has_iterator_type<T> &&
    requires { typename T::iterator::value_type; typename T::iterator::difference_type; };

template<typename T>
concept has_mapped_and_key =
    requires { typename T::mapped_type; typename T::key_type; };

// range_value via iterator (mirror of range_value_t<T> = value_type_t<iterator_traits<iterator_t<T>>>)
template<typename T>
using iter_value_t = typename T::iterator::value_type;

// ===== layer 2: semantic concepts (precise, with B) =====
template<typename B, typename T>
concept array_like =
    has_iterator_traits<T> &&
    !std::same_as<T, iter_value_t<T>> &&              // filesystem::path special case
    std::is_constructible_v<B, iter_value_t<T>>;      // element constructs B

template<typename B, typename T>
concept object_like =
    has_mapped_and_key<T> &&
    std::is_constructible_v<typename B::object_t::key_type, typename T::key_type> &&
    std::is_constructible_v<typename B::object_t::mapped_type, typename T::mapped_type>;

template<typename B, typename T>
concept string_like =
    std::is_constructible_v<typename B::string_t, T>;

template<typename B, typename T>
concept binary_like =
    std::same_as<typename B::binary_t::container_type, T> &&
    !std::same_as<typename B::binary_t::container_type, std::vector<std::uint8_t>>;

// ===== truth source: real library traits =====
template<typename B, typename T> static constexpr bool t_array  = nlohmann::detail::is_compatible_array_type<B, T>::value;
template<typename B, typename T> static constexpr bool t_object = nlohmann::detail::is_compatible_object_type<B, T>::value;
template<typename B, typename T> static constexpr bool t_string = nlohmann::detail::is_compatible_string_type<B, T>::value;
template<typename B, typename T> static constexpr bool t_binary = nlohmann::detail::is_compatible_binary_type<B, T>::value;

// per-trait consensus check: concept must equal the library trait
template<typename T>
bool check_array()
{
    bool ok = array_like<B, T> == t_array<B, T>;
    if (!ok)
    {
        std::printf("  [DRIFT] array<%-14s> trait=%d concept=%d\n", typeid(T).name(), (int)t_array<B, T>, (int)array_like<B, T>);
    }
    return ok;
}
template<typename T>
bool check_object()
{
    bool ok = object_like<B, T> == t_object<B, T>;
    if (!ok)
    {
        std::printf("  [DRIFT] object<%-13s> trait=%d concept=%d\n", typeid(T).name(), (int)t_object<B, T>, (int)object_like<B, T>);
    }
    return ok;
}
template<typename T>
bool check_string()
{
    bool ok = string_like<B, T> == t_string<B, T>;
    if (!ok)
    {
        std::printf("  [DRIFT] string<%-13s> trait=%d concept=%d\n", typeid(T).name(), (int)t_string<B, T>, (int)string_like<B, T>);
    }
    return ok;
}
template<typename T>
bool check_binary()
{
    bool ok = binary_like<B, T> == t_binary<B, T>;
    if (!ok)
    {
        std::printf("  [DRIFT] binary<%-13s> trait=%d concept=%d\n", typeid(T).name(), (int)t_binary<B, T>, (int)binary_like<B, T>);
    }
    return ok;
}

int main()
{
    int bad = 0;
    // type matrix
    using Vint = std::vector<int>;
    using Vstr = std::vector<std::string>;
    using Vjson = std::vector<json>;
    using Str = std::string;
    using Map = std::map<std::string, int>;
    using Set = std::set<int>;
    using Vbyte = std::vector<std::uint8_t>;

    // array
    bad += !check_array<Vint>();
    bad += !check_array<Vstr>();
    bad += !check_array<Vjson>();
    bad += !check_array<Str>();
    bad += !check_array<Map>();
    bad += !check_array<Set>();
    bad += !check_array<Vbyte>();

    // object
    bad += !check_object<Vint>();
    bad += !check_object<Map>();
    bad += !check_object<Str>();

    // string
    bad += !check_string<Str>();
    bad += !check_string<const char*>();
    bad += !check_string<Vint>();

    // binary
    bad += !check_binary<Vbyte>();
    bad += !check_binary<Vint>();

    std::printf(bad ? "LAYERED: %d DRIFTS\n" : "LAYERED: concepts == traits (zero drift)\n", bad);
    std::fflush(stdout);
    return bad ? 1 : 0;
}
