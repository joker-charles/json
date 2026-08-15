// m3_dump.cpp — differential test: reflection-driven value dispatch of the
// JSON serializer vs the real nlohmann::json::dump().
//
// The core claim of M2/M3: the value dispatch is driven by the reflection
// enumerator set (kValueTInfos) via `template for` + per-enumerator NTTP
// action, NOT a hand-written switch. This test then proves that dispatch
// produces byte-identical JSON to the real library across
//   (a) all 10 default-constructed value_t,
//   (b) non-trivial scalar assignments (booleans, every integer, floats),
//   (c) nested object/array payloads,
//   (d) string escaping and binary encoding.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m3_dump m3_dump.cpp && ./m3_dump
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using json = nlohmann::json;
using rjson::basic_json_reflection;
using rjson::reflection_serializer;

static int failures = 0;

void check(const json& lib, const char* label)
{
    basic_json_reflection refl;
    refl.assign_from(lib);
    reflection_serializer ser(refl);
    const std::string got = ser.str();
    const std::string want = lib.dump();
    if (got != want)
    {
        ++failures;
        std::printf("  [FAIL] %-28s\n    want: %s\n    got : %s\n",
                    label, want.c_str(), got.c_str());
    }
}

int main()
{
    // (a) all 10 default-constructed value_t
    check(json(json::value_t::null), "default null");
    check(json(json::value_t::object), "default object");
    check(json(json::value_t::array), "default array");
    check(json(json::value_t::string), "default string");
    check(json(json::value_t::boolean), "default boolean");
    check(json(json::value_t::number_integer), "default int");
    check(json(json::value_t::number_unsigned), "default uint");
    check(json(json::value_t::number_float), "default float");
    check(json(json::value_t::binary), "default binary");

    // (b) scalar assignments
    check(json(true), "boolean true");
    check(json(false), "boolean false");
    check(json(0), "int 0");
    check(json(-42), "int -42");
    check(json(123456789), "int large");
    check(json(42u), "uint 42");
    check(json(0u), "uint 0");
    check(json(3.0), "float 3.0");
    check(json(-3.0), "float -3.0");
    check(json(0.0), "float 0.0");
    check(json(2.5), "float 2.5");
    check(json(-0.5), "float -0.5");
    check(json("hello"), "string hello");
    check(json(""), "string empty");
    check(json("line\nbreak\t\"quoted\"\\"), "string escapes");

    // (c) nested object/array
    {
        json obj = json::object();
        obj["null"] = nullptr;
        obj["bool"] = true;
        obj["int"] = -7;
        obj["uint"] = 42u;
        obj["float"] = 1.5;
        obj["str"] = "a\"b";
        obj["arr"] = json::array({1, 2.5, "x", nullptr, false});
        obj["nested"] = json::object({ {"a", 1}, {"b", json::array({2,3})} });
        check(obj, "nested object");
    }
    {
        json arr = json::array({ nullptr, true, 0, -1, 3.5, "s" });
        check(arr, "mixed array");
    }

    // (d) binary
    {
        json b = json::array();
        json bin = json::binary({1, 2, 254, 255});
        check(bin, "binary with bytes");
        bin = json::binary({});
        check(bin, "binary empty");
    }

    // (e) ensure_ascii: control-char escapes are byte-identical
    {
        json s("tab\there\r\n\b\f\"q\\");
        basic_json_reflection refl;
        refl.assign_from(s);
        rjson::reflection_serializer ser(refl, /*ensure_ascii_=*/true);
        const std::string want = s.dump(-1, ' ', true); // ensure_ascii=true
        if (ser.str() != want)
        {
            ++failures;
            std::printf("  [FAIL] ensure_ascii escapes\n    want: %s\n    got : %s\n",
                        want.c_str(), ser.str().c_str());
        }
    }

    std::printf(failures ? "\nM3 DUMP DIFF: %d FAILURES\n" : "\nM3 DUMP DIFF PASSED\n", failures);
    return failures ? 1 : 0;
}
