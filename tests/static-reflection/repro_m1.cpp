// M1 repro: reflect value_t-like enum with P2996 on GCC 16
//
// Validates (all patterns from the verified `modern-cpp` skill, g++-16
// 16.1.0 / -std=c++26 -freflection):
//   1. std::meta::enumerators_of(^^T) + direct subscript/size on the call
//      (transient-vector rule: never bind the vector to a local constexpr);
//   2. identifier_of for the enumerator-name table;
//   3. value splice via range-based `template for` over
//      std::define_static_array(...)  --  `auto e = [: r :]` then cast;
//   4. a compile-time generated sorting table mirroring value_t.hpp order[].
//
// Build: g++-16 -std=c++26 -freflection -O2 -o repro_m1 repro_m1.cpp
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <array>
#include <utility> // index_sequence, make_index_sequence

#include <meta>

// Mirror of nlohmann::detail::value_t (including the `discarded` enumerator)
enum class value_t : std::uint8_t
{
    null,
    object,
    array,
    string,
    boolean,
    number_integer,
    number_unsigned,
    number_float,
    binary,
    discarded
};

namespace refl
{

consteval std::size_t enum_count()
{
    return std::meta::enumerators_of(^^value_t).size();
}

// Identifier of the I-th enumerator. Subscript the CALL directly; do not bind
// `enumerators_of(...)` to a local constexpr first (transient-vector rule).
consteval std::string_view name_at(std::size_t i)
{
    return std::meta::identifier_of(std::meta::enumerators_of(^^value_t)[i]);
}

// The Python-like sort weight, mirroring value_t.hpp order[] (lines 86-91):
//   null=0, boolean=1, number_*=2, object=3, array=4, string=5, binary=6,
//   discarded is not in the table (weight -1, i.e. unordered).
consteval int sort_weight(std::string_view id)
{
    if (id == "null")
    {
        return 0;
    }
    if (id == "boolean")
    {
        return 1;
    }
    if (id == "number_integer" || id == "number_unsigned" || id == "number_float")
    {
        return 2;
    }
    if (id == "object")
    {
        return 3;
    }
    if (id == "array")
    {
        return 4;
    }
    if (id == "string")
    {
        return 5;
    }
    if (id == "binary")
    {
        return 6;
    }
    return -1; // discarded
}

template<std::size_t... I>
consteval auto names_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)> { name_at(I)... };
}

template<std::size_t... I>
consteval auto weights_impl(std::index_sequence<I...>)
{
    return std::array<int, sizeof...(I)> { sort_weight(name_at(I))... };
}

template<std::size_t N>
consteval auto names_of()
{
    return names_impl(std::make_index_sequence<N> {});
}

template<std::size_t N>
consteval auto weights_of()
{
    return weights_impl(std::make_index_sequence<N> {});
}

} // namespace refl

constexpr auto N       = refl::enum_count();
constexpr auto names   = refl::names_of<N>();
constexpr auto weights = refl::weights_of<N>();

// `template for` range must be a namespace-scope/static constexpr or an inline
// define_static_array(...) expression (a plain local constexpr is rejected).
constexpr auto es = std::define_static_array(std::meta::enumerators_of(^^value_t));

int main()
{
    std::printf("enumerator count = %zu\n", static_cast<std::size_t>(N));
    for (std::size_t i = 0; i < N; ++i)
    {
        std::printf("  [%zu] %-16s weight=%d\n", i,
                    std::string(names[i]).c_str(), weights[i]);
    }

    // value splice: `auto e = [: r :]` then cast (extract<int> on an
    // enumerator throws -- this is the sanctioned route)
    std::printf("value splices:\n");
    template for (constexpr auto r : es)
    {
        auto e = [: r :];
        std::printf("  %-16s -> %u\n",
                    std::string(std::meta::identifier_of(r)).c_str(),
                    static_cast<unsigned>(static_cast<std::uint8_t>(e)));
    }

    // sanity checks vs the library's hand-written order[] table
    bool ok = (N == 10);
    ok = ok && names[0] == "null" && names[1] == "object" && names[2] == "array";
    ok = ok && names[9] == "discarded";
    ok = ok && weights[0] == 0 && weights[2] == 4 && weights[5] == 2 && weights[8] == 6;

    std::printf(ok ? "ALL TYPE-REFLECTION CHECKS PASSED\n" : "MISMATCH\n");
    return ok ? 0 : 1;
}
