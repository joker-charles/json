// probe_real_json_value.cpp — can external reflection reach the REAL
// basic_json::json_value (private nested union) and use it?
//
// Background: AGENTS.md/M4 previously claimed "json_value is private and even
// unchecked() cannot reference it; reflection must target a mirror union".
// That claim is WRONG in an important way: `template for` inline context does
// fail ("not a complete class type") on the indirectly-obtained type, but a
// CONSTEVAL FUNCTION can obtain it and splice it for full use. This probe is
// the counter-evidence: real json_value enumerated, constructed, and members
// read/written — no mirror needed.
//
// Verified pattern (g++-16 16.1.0, -std=c++26 -freflection):
//   * nonstatic_data_members_of(^^json, unchecked()) -> [0] is `data` (private
//     nested struct); [1] is `m_type`; `data`'s members -> [1] is `m_value`.
//     NOTE: rely on identifier_of(name) checks, NOT hardcoded indices, for
//     robustness against member reordering.
//   * type_of(m_value) yields json_value; enumerating ITS members works ONLY
//     from a consteval context (template-for inline says "not a complete
//     class type" — GCC 16 limitation).
//   * splice the obtained info to declare the type and use it fully.
//
// Build: g++-16 -std=c++26 -freflection -O0 -Iinclude -o probe_real_json_value probe_real_json_value.cpp
#include <meta>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <string_view>

using json = nlohmann::json;

// data (private nested struct) info: data is basic_json's first private
// non-static data member under unchecked().
consteval auto data_info()
{
    constexpr auto m_data = std::meta::nonstatic_data_members_of(
                                ^^json, std::meta::access_context::unchecked())[0];
    return std::meta::type_of(m_data);
}
// json_value (private nested union) info: m_value is data's second member
// (m_type is [0], m_value is [1]).
consteval auto json_value_info()
{
    constexpr auto m_value = std::meta::nonstatic_data_members_of(
                                 data_info(), std::meta::access_context::unchecked())[1];
    return std::meta::type_of(m_value);
}

// splice the real type out
using real_json_value = typename [: json_value_info() :];

int main()
{
    // 1. member enumeration from a consteval context (the ONLY working one)
    constexpr std::size_t n = std::meta::nonstatic_data_members_of(
                                  json_value_info(), std::meta::access_context::unchecked()).size();
    std::printf("json_value member count = %zu (expect 8)\n", n);
    template for (constexpr auto m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(json_value_info(), std::meta::access_context::unchecked())))
    {
        std::printf("  - %s\n", std::meta::identifier_of(m).data());
    }

    // 2. full lifecycle: default + value_t ctor, member read/write
    real_json_value v;
    v.number_integer = 42;
    std::printf("number_integer = %lld\n", static_cast<long long>(v.number_integer));

    // value_t::string 构造会分配 string 堆对象（json.hpp:504 create<>）；这里改用手动管理：
    real_json_value vs;                       // 默认构造 = null，无分配
    vs.string = new std::string("hello via reflection");
    std::printf("string = %s\n", vs.string->c_str());
    delete vs.string;                          // 手动释放，与分配配对

    real_json_value vn(json::value_t::number_unsigned);
    vn.number_unsigned = 1234567890123ULL;
    std::printf("number_unsigned = %llu\n", static_cast<unsigned long long>(vn.number_unsigned));

    bool ok = (n == 8) && (v.number_integer == 42) && (vn.number_unsigned == 1234567890123ULL);
    std::printf(ok ? "REAL JSON_VALUE PROBE PASSED\n" : "REAL JSON_VALUE PROBE MISMATCH\n");
    return ok ? 0 : 1;
}
