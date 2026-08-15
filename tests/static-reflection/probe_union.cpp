// Probe: union member reflection + pointer/scalar classification.
//
// Validates the premise of M2: meta::nonstatic_data_members_of works on a
// union (returns only the data members, no member functions), type_of(m)
// spliced into a type context, and std::is_pointer_v classifies storage
// category. Mirror union with the same shape as basic_json::json_value
// (pointer members + scalar members).
//
// Build: g++-16 -std=c++26 -freflection -O2 -o probe_union probe_union.cpp
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include <meta>

struct obj_t { int x; };

union json_value
{
    obj_t* object;
    int* array;
    long long number_integer;
    double number_float;
    bool boolean;
};

consteval std::size_t member_count()
{
    return std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged()).size();
}

template<std::size_t I>
consteval bool member_is_pointer()
{
    constexpr auto m =
        std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[I];
    using M = typename [: std::meta::type_of(m) :];
    return std::is_pointer_v<M>;
}

int main()
{
    constexpr std::size_t N = member_count();
    std::printf("union members = %zu\n", N);

    // reflection subscripts need a constant index: query each member directly
    constexpr auto m0 = std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[0];
    constexpr auto m1 = std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[1];
    constexpr auto m2 = std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[2];
    constexpr auto m3 = std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[3];
    constexpr auto m4 = std::meta::nonstatic_data_members_of(^^json_value, std::meta::access_context::unprivileged())[4];
    std::printf("  [0] %-15s pointer=%d\n", std::string(std::meta::identifier_of(m0)).c_str(), static_cast<int>(member_is_pointer<0>()));
    std::printf("  [1] %-15s pointer=%d\n", std::string(std::meta::identifier_of(m1)).c_str(), static_cast<int>(member_is_pointer<1>()));
    std::printf("  [2] %-15s pointer=%d\n", std::string(std::meta::identifier_of(m2)).c_str(), static_cast<int>(member_is_pointer<2>()));
    std::printf("  [3] %-15s pointer=%d\n", std::string(std::meta::identifier_of(m3)).c_str(), static_cast<int>(member_is_pointer<3>()));
    std::printf("  [4] %-15s pointer=%d\n", std::string(std::meta::identifier_of(m4)).c_str(), static_cast<int>(member_is_pointer<4>()));

    bool ok = (N == 5)
           && member_is_pointer<0>() && member_is_pointer<1>()   // object / array
           && !member_is_pointer<2>() && !member_is_pointer<3>() // numbers
           && !member_is_pointer<4>();                           // boolean
    std::printf(ok ? "UNION REFLECTION PROBE PASSED\n" : "UNION REFLECTION PROBE MISMATCH\n");
    return ok ? 0 : 1;
}
