// m3_binary.cpp — differential test: reflection-driven CBOR writer vs the real
// nlohmann::json::to_cbor().
//
// Same dispatch claim as m3_dump.cpp (value routing from the reflection
// enumerator set), but for the binary wire format. Byte-level encoding must
// equal the library's CBOR output across:
//   (a) all default-constructed value_t;
//   (b) scalar assignments incl. negative integers and floats of varied width;
//   (c) nested containers;
//   (d) binary with/without subtype.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m3_binary m3_binary.cpp && ./m3_binary
#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using json = nlohmann::json;
using rjson::basic_json_reflection;
using rjson::reflection_cbor_serializer;

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

void check(const json& lib, const char* label)
{
    basic_json_reflection refl;
    refl.assign_from(lib);
    reflection_cbor_serializer ser(refl);
    const auto got = ser.bytes();
    const auto want = json::to_cbor(lib);
    if (got != want)
    {
        ++failures;
        std::printf("  [FAIL] %-28s\n    want: %s\n    got : %s\n",
                    label, hex(want).c_str(), hex(got).c_str());
    }
}

int main()
{
    // (a) default value_t
    check(json(json::value_t::null), "default null");
    check(json(json::value_t::object), "default object");
    check(json(json::value_t::array), "default array");
    check(json(json::value_t::string), "default string");
    check(json(json::value_t::boolean), "default boolean");
    check(json(json::value_t::number_integer), "default int");
    check(json(json::value_t::number_unsigned), "default uint");
    check(json(json::value_t::number_float), "default float");
    check(json(json::value_t::binary), "default binary");

    // (b) scalars across width boundaries
    check(json(0), "int 0");
    check(json(23), "int 23");
    check(json(24), "int 24");
    check(json(255), "int 255");
    check(json(256), "int 256");
    check(json(65535), "int 65535");
    check(json(65536), "int 65536");
    check(json(-1), "int -1");
    check(json(-24), "int -24");
    check(json(-25), "int -25");
    check(json(-256), "int -256");
    check(json(-100000), "int -100000");
    check(json(42u), "uint 42");
    check(json(0.0), "float 0.0");
    check(json(1.0), "float 1.0");
    check(json(0.5), "float 0.5");
    check(json(2.5), "float 2.5");
    check(json(true), "bool true");
    check(json(false), "bool false");

    // (c) nested containers
    {
        json obj = json::object();
        obj["a"] = 1;
        obj["b"] = json::array({1, 2, 3});
        obj["c"] = "text";
        obj["d"] = json::object({ {"x", -5}, {"y", 2.5} });
        check(obj, "nested object");
    }
    {
        json arr = json::array({ nullptr, true, 0, -42, 3.5, "s", json::array({1}) });
        check(arr, "mixed array");
    }

    // (d) binary with and without subtype
    check(json::binary({1, 2, 3, 250, 255}), "binary bytes");
    check(json::binary({}), "binary empty");
    check(json::binary({5}, 7), "binary with subtype");
    check(json::binary({1, 2, 3, 4}, 300), "binary subtype u16");

    std::printf(failures ? "\nM3 CBOR DIFF: %d FAILURES\n" : "\nM3 CBOR DIFF PASSED\n", failures);
    return failures ? 1 : 0;
}
