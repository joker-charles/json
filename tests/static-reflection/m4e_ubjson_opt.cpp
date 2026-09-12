// m4e_ubjson_opt.cpp — differential test: the optimized UBJSON modes + BJData
// writer (reflection_ubjson_optimized_serializer) vs the real library's
// to_ubjson / to_bjdata.
//
// Covers (g++-16, -std=c++26 -freflection):
//   * UBJSON with all three legal {use_count, use_type} combinations
//     (use_type requires use_count, as in the library: JSON_ASSERT);
//   * BJData draft2 and draft3 for the same combinations — the little-endian
//     numeric payloads, the 'u'/'m'/'M' width rungs, the '$' type
//     optimization with its bjdx exclusion list, and the draft3 'B' binary
//     marker;
//   * JData ndarray objects (_ArrayType_/_ArraySize_/_ArrayData_) in BJData
//     (valid encodings and invalid fallbacks), which stay plain objects in
//     UBJSON;
//   * width boundaries for numbers, string/key lengths, and container counts
//     (int8..uint64, fix ranges, 256/65536 element counts);
//   * the no-optimization cross-check: the new writer with (F,F) must equal
//     the existing reflection_ubjson_serializer bytes and to_ubjson(F,F).
// Every comparison is byte-identical vs the real library.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m4e_ubjson_opt m4e_ubjson_opt.cpp && ./m4e_ubjson_opt
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using lib_json = nlohmann::json;
using value_t  = nlohmann::detail::value_t;
using rjson::basic_json_reflection;
using rjson::reflection_ubjson_optimized_serializer;
using rjson::reflection_ubjson_serializer;

bool ok = true;
std::size_t checks = 0;

void report_fail(const std::string& what, const std::string& detail = "")
{
    ok = false;
    std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

#define CHECK(cond, msg, ...)                                                                  \
    do                                                                                         \
    {                                                                                          \
        ++checks;                                                                              \
        if (!(cond))                                                                           \
        {                                                                                      \
            char buf[256];                                                                     \
            std::snprintf(buf, sizeof buf, msg __VA_OPT__(, ) __VA_ARGS__);                    \
            report_fail(buf);                                                                  \
        }                                                                                      \
    } while (0)

// hex dump for failure messages
std::string hex(const std::vector<std::uint8_t>& v)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (const auto b : v)
    {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 0xF]);
    }
    return s;
}

const std::vector<lib_json> kCases =
{
    // scalars
    lib_json(nullptr),
    lib_json(true),
    lib_json(false),
    lib_json(0),
    lib_json(1),
    lib_json(-1),
    lib_json(127),
    lib_json(128),
    lib_json(-128),
    lib_json(-129),
    lib_json(255),
    lib_json(256),
    lib_json(32767),
    lib_json(32768),
    lib_json(-32768),
    lib_json(-32769),
    lib_json(65535u),            // bjdata 'u' rung (unsigned)
    lib_json(65536u),
    lib_json(2147483647),
    lib_json(-2147483648),
    lib_json(2147483648u),
    lib_json(4294967295u),       // bjdata 'm' rung
    lib_json(4294967296ull),
    lib_json(9223372036854775807ll),
    lib_json(-9223372036854775807ll - 1),
    lib_json(18446744073709551615ull), // bjdata 'M'; plain UBJSON -> 'H'
    lib_json(3.5),
    lib_json(-0.0),
    lib_json(1e100),
    lib_json(""),
    lib_json("a"),
    lib_json("hello"),
    lib_json(std::string(300, 'x')),
    lib_json(std::string(65536, 'y')),
    // containers
    lib_json::array(),
    lib_json::object(),
    lib_json::array({1, 2, 3}),                 // homogeneous int8 -> '$i'
    lib_json::array({1, 2, 300}),               // mixed widths -> no '$'
    lib_json::array({1, "a"}),                  // heterogeneous
    lib_json::array({lib_json::array({1}), lib_json::array({2})}), // '$[' — excluded in BJData
    lib_json::array({"a", "b"}),                // '$S' — excluded in BJData
    lib_json::array({true, true}),              // '$T' — excluded in BJData
    lib_json::array({lib_json::array({}), lib_json::array({1, 2, 3})}),
    lib_json::object({{"a", 1}, {"b", 2}}),     // homogeneous values -> '$i'
    lib_json::object({{"a", 1}, {"b", "x"}}),
    lib_json{{"k", lib_json::array({1, 2, 3})}},
    lib_json::array({lib_json::array({1, 2}), lib_json::object({{"x", 1}})}),
    lib_json::object({{"long key name here", lib_json::array({true, false, nullptr})}}),
    // 256 elements: count prefix crosses the 'i'/'U' boundary
    ([] { lib_json a = lib_json::array(); for (int i = 0; i < 256; ++i) a.push_back(i % 100); return a; })(),
    // binary (draft2/draft3 'B'/'U' markers)
    lib_json::binary({1, 2, 3}),
    lib_json::binary({1, 2, 3}, 7),
    lib_json::binary({}),
    // JData ndarray: valid
    lib_json::object({{"_ArrayType_", "double"}, {"_ArraySize_", lib_json::array({2, 3})},
        {"_ArrayData_", lib_json::array({1.5, 2.5, 3.5, 4.5, 5.5, 6.5})}}),
    lib_json::object({{"_ArrayType_", "uint8"}, {"_ArraySize_", lib_json::array({3})},
        {"_ArrayData_", lib_json::array({1, 2, 3})}}),
    lib_json::object({{"_ArrayType_", "int16"}, {"_ArraySize_", lib_json::array({2, 2})},
        {"_ArrayData_", lib_json::array({-1, 2, -3, 4})}}),
    lib_json::object({{"_ArrayType_", "uint16"}, {"_ArraySize_", lib_json::array({2})},
        {"_ArrayData_", lib_json::array({1, 65535})}}),
    lib_json::object({{"_ArrayType_", "uint64"}, {"_ArraySize_", lib_json::array({1})},
        {"_ArrayData_", lib_json::array({18446744073709551615ull})}}),
    lib_json::object({{"_ArrayType_", "byte"}, {"_ArraySize_", lib_json::array({3})},
        {"_ArrayData_", lib_json::array({1, 2, 255})}}),
    // JData ndarray: invalid -> falls back to a plain object
    lib_json::object({{"_ArrayType_", "nope"}, {"_ArraySize_", lib_json::array({1})},
        {"_ArrayData_", lib_json::array({1})}}),
    lib_json::object({{"_ArrayType_", "uint8"}, {"_ArraySize_", lib_json::array({2})},
        {"_ArrayData_", lib_json::array({1})}}), // size mismatch
    lib_json::object({{"_ArrayType_", "uint8"}, {"_ArraySize_", lib_json::array({1})},
        {"_ArrayData_", lib_json::array({"x"})}}), // wrong element kind
    lib_json::object({{"_ArrayType_", "uint8"}, {"_ArraySize_", lib_json::array({-1})},
        {"_ArrayData_", lib_json::array({1})}}), // negative dimension
    lib_json(value_t::discarded),
};

void run_case(const lib_json& c)
{
    basic_json_reflection refl;
    refl.assign_from(c);

    // cross-check: (F,F) must equal the existing no-optimization writer
    {
        const auto mine = reflection_ubjson_optimized_serializer(refl, false, false, false, false).bytes();
        const auto old  = reflection_ubjson_serializer(refl).bytes();
        const auto want = lib_json::to_ubjson(c, false, false);
        CHECK(mine == want && mine == old,
              "ubjson(F,F) mismatch for %s (mine=%s want=%s old=%s)",
              c.dump().c_str(), hex(mine).c_str(), hex(want).c_str(), hex(old).c_str());
    }

    for (const bool uc :
            {
                false, true
            })
    {
        for (const bool ut :
                {
                    false, true
                })
        {
            if (ut && !uc)
            {
                continue; // the library asserts use_count for use_type
            }

            // plain UBJSON
            {
                const auto mine = reflection_ubjson_optimized_serializer(refl, uc, ut, false, false).bytes();
                const auto want = lib_json::to_ubjson(c, uc, ut);
                CHECK(mine == want, "ubjson(uc=%d,ut=%d) mismatch for %s (mine=%s want=%s)",
                      uc, ut, c.dump().c_str(), hex(mine).c_str(), hex(want).c_str());
            }

            // BJData draft2
            {
                const auto mine = reflection_ubjson_optimized_serializer(refl, uc, ut, true, false).bytes();
                const auto want = lib_json::to_bjdata(c, uc, ut, nlohmann::detail::bjdata_version_t::draft2);
                CHECK(mine == want, "bjdata-d2(uc=%d,ut=%d) mismatch for %s (mine=%s want=%s)",
                      uc, ut, c.dump().c_str(), hex(mine).c_str(), hex(want).c_str());
            }

            // BJData draft3
            {
                const auto mine = reflection_ubjson_optimized_serializer(refl, uc, ut, true, true).bytes();
                const auto want = lib_json::to_bjdata(c, uc, ut, nlohmann::detail::bjdata_version_t::draft3);
                CHECK(mine == want, "bjdata-d3(uc=%d,ut=%d) mismatch for %s (mine=%s want=%s)",
                      uc, ut, c.dump().c_str(), hex(mine).c_str(), hex(want).c_str());
            }
        }
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[1] %zu cases x (ubjson + 3 bjdata modes) byte-identical vs the real library\n",
                kCases.size());

    for (std::size_t i = 0; i < kCases.size(); ++i)
    {
        run_case(kCases[i]);
    }

    std::printf("\nM4E UBJSON-OPT DIFF TEST %s (%zu checks)\n", ok ? "PASSED" : "FAILED", checks);
    return ok ? 0 : 1;
}
