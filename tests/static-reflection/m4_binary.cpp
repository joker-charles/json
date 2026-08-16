// m4_binary.cpp — differential test: reflection-driven MSGPACK / UBJSON /
// BSON writers vs the real nlohmann::json::{to_msgpack,to_ubjson,to_bson}().
//
// Same dispatch claim as m3_binary.cpp (value routing from the reflection
// enumerator set, kValueTInfos), extended to the three remaining binary
// formats of the value_t -> byte-code table milestone: each writer's primary
// byte codes come from the reflection-generated kMsgpackCodes / kUbjsonCodes
// / kBsonCodes tables; range-dependent forms (fixnum/fixstr/fixarray/fixmap,
// 8/16/32 width selection, UBJSON number narrowing, BSON int32/int64/uint64)
// are selected inside the per-enumerator actions.
//
// UBJSON is compared in the no-optimization mode (use_count=false,
// use_type=false — the library's to_ubjson defaults); the optimized modes
// ('#'/'\$' prefixes, BJData) are not implemented in the reflection writers.
// BSON requires a top-level object (the library throws type_error 317
// otherwise — the reflection writer replicates the throw).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m4_binary m4_binary.cpp && ./m4_binary
#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using json = nlohmann::json;
using rjson::basic_json_reflection;
using rjson::reflection_msgpack_serializer;
using rjson::reflection_ubjson_serializer;
using rjson::reflection_bson_serializer;

static int failures = 0;

static std::string hex(const std::vector<std::uint8_t>& v)
{
    std::string s = "[";
    for (auto c : v)
    {
        char b[4];
        std::snprintf(b, sizeof b, "%02x ", c);
        s += b;
    }
    if (!v.empty())
    {
        s.pop_back();
    }
    s += "]";
    return s;
}

static void check(const json& lib, const char* label)
{
    basic_json_reflection refl;
    refl.assign_from(lib);

    const auto got_m = reflection_msgpack_serializer(refl).bytes();
    const auto want_m = json::to_msgpack(lib);
    if (got_m != want_m)
    {
        ++failures;
        std::printf("  [FAIL] %-30s msgpack\n    want: %s\n    got : %s\n",
                    label, hex(want_m).c_str(), hex(got_m).c_str());
    }

    const auto got_u = reflection_ubjson_serializer(refl).bytes();
    const auto want_u = json::to_ubjson(lib);
    if (got_u != want_u)
    {
        ++failures;
        std::printf("  [FAIL] %-30s ubjson\n    want: %s\n    got : %s\n",
                    label, hex(want_u).c_str(), hex(got_u).c_str());
    }

    if (lib.is_object())
    {
        const auto got_b = reflection_bson_serializer(refl).bytes();
        const auto want_b = json::to_bson(lib);
        if (got_b != want_b)
        {
            ++failures;
            std::printf("  [FAIL] %-30s bson\n    want: %s\n    got : %s\n",
                        label, hex(want_b).c_str(), hex(got_b).c_str());
        }
    }
}

// BSON: top-level non-object must throw (library type_error 317 semantics)
static void check_bson_throw(const json& lib, const char* label)
{
    basic_json_reflection refl;
    refl.assign_from(lib);
    try
    {
        (void)reflection_bson_serializer(refl).bytes();
        ++failures;
        std::printf("  [FAIL] %-30s bson (expected throw, got output)\n", label);
    }
    catch (const std::exception& e)
    {
        std::printf("  [ ok ] %-30s bson throws: %s\n", label, e.what());
    }
}

int main()
{
    std::printf("m4_binary — msgpack/ubjson/bson differential (vs real library)\n\n");

    std::printf("[a] default-constructed value_t\n");
    check(json(json::value_t::null), "default null");
    check(json(json::value_t::object), "default object");
    check(json(json::value_t::array), "default array");
    check(json(json::value_t::string), "default string");
    check(json(json::value_t::boolean), "default boolean");
    check(json(json::value_t::number_integer), "default int");
    check(json(json::value_t::number_unsigned), "default uint");
    check(json(json::value_t::number_float), "default float");
    check(json(json::value_t::binary), "default binary");

    std::printf("[b] integers across the width boundaries\n");
    check(json(0), "int 0");
    check(json(1), "int 1");
    check(json(23), "int 23 (fixnum edge)");
    check(json(24), "int 24 (u8 edge)");
    check(json(127), "int 127 (i8 max)");
    check(json(128), "int 128 (u8 edge)");
    check(json(255), "int 255 (u8 max)");
    check(json(256), "int 256 (u16 edge)");
    check(json(65535), "int 65535 (u16 max)");
    check(json(65536), "int 65536 (u32 edge)");
    check(json(-1), "int -1 (neg fixnum)");
    check(json(-31), "int -31 (neg fixnum)");
    check(json(-32), "int -32 (neg fixnum edge)");
    check(json(-33), "int -33 (i8 edge)");
    check(json(-127), "int -127");
    check(json(-128), "int -128 (i8 min)");
    check(json(-129), "int -129 (i16 edge)");
    check(json(-32767), "int -32767");
    check(json(-32768), "int -32768 (i16 min)");
    check(json(-32769), "int -32769 (i32 edge)");
    check(json(2147483647LL), "int 2147483647 (i32 max)");
    check(json(2147483648LL), "int 2147483648 (u32 edge)");
    check(json(4294967295ULL), "uint 4294967295 (u32 max)");
    check(json(4294967296ULL), "uint 4294967296 (u64 edge)");
    check(json(9223372036854775807LL), "int 9223372036854775807 (i64 max)");
    check(json(18446744073709551615ULL), "uint 18446744073709551615 (u64 max)");

    std::printf("[c] floats (compact float32 vs float64, non-finite)\n");
    check(json(0.0), "float 0.0");
    check(json(1.5), "float 1.5");
    check(json(-2.25), "float -2.25");
    check(json(3.4028234663852886e38), "float ~float32 max (f32 exact)");
    check(json(1.0e100), "float 1e100 (needs f64)");
    check(json(std::numeric_limits<double>::infinity()), "float +inf");
    check(json(-std::numeric_limits<double>::infinity()), "float -inf");
    check(json(std::numeric_limits<double>::quiet_NaN()), "float nan");

    std::printf("[d] strings across the length boundaries\n");
    check(json(""), "string empty");
    check(json(std::string(31, 'a')), "string len 31 (fixstr edge)");
    check(json(std::string(32, 'a')), "string len 32 (str8 edge)");
    check(json(std::string(255, 'b')), "string len 255 (str8 max)");
    check(json(std::string(256, 'c')), "string len 256 (str16 edge)");
    check(json(std::string(65535, 'd')), "string len 65535 (str16 max)");

    std::printf("[e] containers (fixarray/fixmap and width edges)\n");
    json a15 = json::array();
    json a16 = json::array();
    for (int i = 0; i < 15; ++i)
    {
        a15.push_back(i);
    }
    for (int i = 0; i < 16; ++i)
    {
        a16.push_back(i);
    }
    check(a15, "array size 15 (fixarray edge)");
    check(a16, "array size 16 (array16 edge)");

    json o15 = json::object();
    json o16 = json::object();
    for (int i = 0; i < 15; ++i)
    {
        o15[std::to_string(i)] = i;
    }
    for (int i = 0; i < 16; ++i)
    {
        o16[std::to_string(i)] = i;
    }
    check(o15, "object size 15 (fixmap edge)");
    check(o16, "object size 16 (map16 edge)");

    check(json::array({json::array({1, 2}), json::object({{"k", "v"}}), nullptr}), "nested containers");
    check(json::object({{"a", json::array({1.5, "x", true})}, {"b", json::object({{"c", nullptr}})}}), "nested object");

    std::printf("[f] binary (bin/ext, subtype, fixext edges)\n");
    check(json::binary({}), "binary empty");
    check(json::binary({1, 2, 3}), "binary no subtype");
    json b1 = json::binary({0xAA});
    b1.get_binary().set_subtype(42);
    check(b1, "binary subtype 42 len 1 (fixext1)");
    json b2 = json::binary({0, 0});
    b2.get_binary().set_subtype(7);
    check(b2, "binary subtype len 2 (fixext2)");
    json b4 = json::binary({0, 0, 0, 0});
    b4.get_binary().set_subtype(7);
    check(b4, "binary subtype len 4 (fixext4)");
    json b8 = json::binary(std::vector<std::uint8_t>(8, 0));
    b8.get_binary().set_subtype(7);
    check(b8, "binary subtype len 8 (fixext8)");
    json b16 = json::binary(std::vector<std::uint8_t>(16, 0));
    b16.get_binary().set_subtype(7);
    check(b16, "binary subtype len 16 (fixext16)");
    json b17 = json::binary(std::vector<std::uint8_t>(17, 0));
    b17.get_binary().set_subtype(7);
    check(b17, "binary subtype len 17 (ext8)");
    check(json::binary(std::vector<std::uint8_t>(256, 0x42)), "binary len 256 (bin16)");

    std::printf("[g] BSON-specific: integer widths, embedded documents\n");
    check(json::object({{"i32", 42}}), "bson int32");
    check(json::object({{"i64", 3000000000LL}}), "bson int64");
    check(json::object({{"u64", 18446744073709551615ULL}}), "bson uint64");
    check(json::object({{"d", 1.5}}), "bson double");
    check(json::object({{"s", "text"}}), "bson string");
    check(json::object({{"b", true}, {"n", nullptr}}), "bson bool+null");
    check(json::object({{"arr", json::array({1, "x", nullptr})}}), "bson embedded array");
    check(json::object({{"obj", json::object({{"in", 1}})}}), "bson embedded object");
    check(json::object({{"bin", json::binary({1, 2, 3})}}), "bson binary");
    json bs = json::binary({9});
    bs.get_binary().set_subtype(3);
    check(json::object({{"bin", bs}}), "bson binary subtype");
    check(json::object({{"key_" + std::string(127, 'x'), 1}}), "bson long key");
    check(json::object({{"nest", json::object({{"deep", json::array({json::object({{"x", 1}})})}})}}), "bson deep nest");

    std::printf("[h] BSON top-level must be object (throw)\n");
    check_bson_throw(json(nullptr), "bson top null");
    check_bson_throw(json(1), "bson top int");
    check_bson_throw(json("s"), "bson top string");
    check_bson_throw(json::array({1}), "bson top array");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "M4 PROBE PASS" : "M4 PROBE FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
