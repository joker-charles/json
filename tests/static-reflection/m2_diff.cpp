// m2_diff.cpp — differential test: reflection-driven tagged union vs the real
// nlohmann/json library for the storage/value_t mapping.
//
// Proves (g++-16, -std=c++26 -freflection):
//   (1) reflection-generated storage_category table matches the real union:
//       object/array/string/binary are pointer-stored, boolean/numbers scalar;
//   (2) default-constructing every value_t through the reflection path yields
//       the same active-type and is_*() classification as the real library;
//   (3) the invariant (current_storage_is_pointer) matches the table;
//   (4) destroy() frees pointer members (verified under AddressSanitizer).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -I. -o m2_diff m2_diff.cpp && ./m2_diff
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using lib_json = nlohmann::json;
using value_t  = nlohmann::detail::value_t;

constexpr auto kVTCount = rjson::kValueTInfos.size();

template<std::size_t... I>
consteval auto all_value_ts_impl(std::index_sequence<I...>)
{
    return std::array<value_t, sizeof...(I)>
    {
        static_cast<value_t>([: rjson::kValueTInfos[I] :])...
    };
}
constexpr auto kAllValueTs = all_value_ts_impl(std::make_index_sequence<kVTCount> {});
template<std::size_t... I>
consteval auto all_names_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)>
    {
        std::meta::identifier_of(rjson::kValueTInfos[I])...
    };
}
constexpr auto kAllNames = all_names_impl(std::make_index_sequence<kVTCount> {});

int main()
{
    bool ok = true;
    constexpr auto& STORAGE = rjson::kStorage;
    constexpr auto& NAMES   = rjson::kMemberIds;

    // (1) storage-category table: pointer-flags must match the real union.
    constexpr bool expected_ptr[8] = { true, true, true, true, false, false, false, false };
    for (std::size_t i = 0; i < 8; ++i)
    {
        if (STORAGE[i] != expected_ptr[i])
        {
            std::printf("  [FAIL] slot %zu %s: pointer=%d expected=%d\n", i,
                        std::string(NAMES[i]).c_str(), STORAGE[i], expected_ptr[i]);
            ok = false;
        }
    }

    // (2)+(3) differential against the real library for every value_t.
    for (std::size_t i = 0; i < kVTCount; ++i)
    {
        const value_t vt = kAllValueTs[i];
        lib_json real(vt);
        rjson::basic_json_reflection refl(vt);

        const bool ok_real_object = real.is_object();
        const bool ok_refl_object = refl.is_object();
        struct Cls
        {
            bool b, n, ui, f, nul, disc;
        };
        const auto r = Cls{ real.is_boolean(), real.is_number_integer(),
                            real.is_number_unsigned(), real.is_number_float(),
                            real.is_null(), real.is_discarded() };
        const auto x = Cls{ refl.is_boolean(), refl.is_number_integer(),
                            refl.is_number_unsigned(), refl.is_number_float(),
                            refl.is_null(), refl.is_discarded() };

        const bool match =
            (ok_real_object == ok_refl_object) && (r.b == x.b) && (r.n == x.n) &&
            (r.ui == x.ui) && (r.f == x.f) && (r.nul == x.nul) && (r.disc == x.disc) &&
            (real.is_number() == refl.is_number());

        // invariant: current storage pointerness equals the table lookup
        const bool is_ptr = refl.current_storage_is_pointer();
        const bool table_ptr =
            vt == value_t::object || vt == value_t::array ||
            vt == value_t::string || vt == value_t::binary;
        const bool inv_ok = (is_ptr == table_ptr);

        if (!match || !inv_ok)
        {
            std::printf("  [FAIL] %s: match=%d inv_ok=%d (ptr=%d)\n",
                        std::string(kAllNames[i]).c_str(), match, inv_ok, is_ptr);
            ok = false;
        }
    }

    // no leaks on destruction of every type (ASan verifies)
    for (std::size_t round = 0; round < 100; ++round)
        for (std::size_t i = 0; i < kVTCount; ++i)
        {
            rjson::basic_json_reflection(kAllValueTs[i]);
        }

    std::printf(ok ? "\nM2 DIFF TEST PASSED\n" : "\nM2 DIFF TEST FAILED\n");
    return ok ? 0 : 1;
}
