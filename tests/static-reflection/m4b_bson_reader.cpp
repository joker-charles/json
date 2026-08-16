// m4b_bson_reader.cpp — differential test: the reflection-driven BSON reader
// (rjson::reflection_bson_parser, M4B-2) vs the real
// nlohmann::json::from_bson().
//
// The reader is the mirror image of the M4B writer: element-type dispatch
// comes from the reflection-generated kBsonsLoad table (byte code ->
// {union slot, payload kind}); document framing, length prefixes and error
// handling are plain mechanics. This probe proves the READ direction is
// byte-identical to the library on legal input:
//
//   for each test value `lib` (top-level object):
//     bytes  = json::to_bson(lib)                  (library BSON bytes)
//     want   = json::from_bson(bytes).dump()       (library parse result)
//     got    = rjson parse of the SAME bytes -> dump
//     assert got == want
//
// BSON binaries always carry a subtype byte, so the round-trip baseline is
// from_bson(to_bson(lib)) — both sides see the same subtype semantics.
// Also checks the rjson writer<->reader round-trip (self-consistency) and
// that malformed input fails safely (returns false + a diagnostic), without
// promising library-identical error codes (documented boundary).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m4b_bson_reader m4b_bson_reader.cpp
//   && ./m4b_bson_reader
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using json = nlohmann::json;
using rjson::basic_json_reflection;
using rjson::reflection_bson_serializer;
using rjson::reflection_bson_parser;

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
    const auto bytes = json::to_bson(lib);
    const std::string want = json::from_bson(bytes).dump();

    basic_json_reflection parsed;
    reflection_bson_parser parser(bytes);
    bool ok = parser.parse(parsed);
    const std::string got = ok ? rjson::reflection_serializer(parsed).str() : std::string("<parse failed: ") + parser.error() + ">";

    if (!ok || got != want)
    {
        ++failures;
        std::printf("  [FAIL] %-32s\n    want: %s\n    got : %s\n    err : %s\n",
                    label, want.c_str(), got.c_str(), parser.error().c_str());
        return;
    }

    // rjson writer -> rjson reader self-consistency: serializing the parsed
    // value again must round-trip (idempotent on the byte level)
    const auto bytes2 = reflection_bson_serializer(parsed).bytes();
    if (bytes2 != bytes)
    {
        ++failures;
        std::printf("  [FAIL] %-32s rjson round-trip bytes differ\n    want: %s\n    got : %s\n",
                    label, hex(bytes).c_str(), hex(bytes2).c_str());
        return;
    }
    std::printf("  [ ok ] %-32s %s\n", label, want.c_str());
}

// malformed input must fail safely (false + diagnostic), never crash
static void check_negative(const std::vector<std::uint8_t>& bytes, const char* label)
{
    basic_json_reflection parsed;
    reflection_bson_parser parser(bytes);
    const bool ok = parser.parse(parsed);
    if (ok)
    {
        ++failures;
        std::printf("  [FAIL] %-32s expected parse failure, got success\n", label);
        return;
    }
    if (parser.error().empty())
    {
        ++failures;
        std::printf("  [FAIL] %-32s failed without a diagnostic\n", label);
        return;
    }
    std::printf("  [ ok ] %-32s fails safely: %s\n", label, parser.error().c_str());
}

int main()
{
    std::printf("m4b_bson_reader — BSON read differential (vs real library) + negatives\n\n");

    std::printf("[a] scalar element types\n");
    check(json::object({{"k", nullptr}}), "null");
    check(json::object({{"k", true}}), "boolean true");
    check(json::object({{"k", false}}), "boolean false");
    check(json::object({{"k", 0}}), "int32 0");
    check(json::object({{"k", 42}}), "int32 42");
    check(json::object({{"k", -42}}), "int32 -42");
    check(json::object({{"k", 2147483647}}), "int32 max");
    check(json::object({{"k", -2147483648LL}}), "int32 min");
    check(json::object({{"k", 2147483648LL}}), "int64 beyond int32");
    check(json::object({{"k", -2147483649LL}}), "int64 below int32");
    check(json::object({{"k", 9223372036854775807LL}}), "int64 max");
    check(json::object({{"k", 4294967296ull}}), "uint64 beyond int64");
    check(json::object({{"k", 18446744073709551615ull}}), "uint64 max");
    check(json::object({{"k", 2.5}}), "double 2.5");
    check(json::object({{"k", -0.0}}), "double -0.0");
    check(json::object({{"k", 1e100}}), "double 1e100");
    check(json::object({{"k", ""}}), "empty string");
    check(json::object({{"k", "hello"}}), "string");
    check(json::object({{"k", std::string("a\x00" "b", 3)}}), "string with NUL");
    check(json::object({{"k", json::binary_t(std::vector<std::uint8_t>{1, 2, 3, 255})}}), "binary subtype default");
    {
        json::binary_t b(std::vector<std::uint8_t>{0x10, 0x20});
        b.set_subtype(0xFF);
        check(json::object({{"k", b}}), "binary subtype 0xFF");
    }

    std::printf("\n[b] structures\n");
    check(json::object(), "empty object");
    check(json::object({{"a", 1}, {"b", "two"}, {"c", 3.0}}), "mixed object");
    // {0x05,0,0,0,0}: size 5 = 4-byte size + 1-byte terminator — a LEGAL empty document
    {
        const std::vector<std::uint8_t> min_doc{0x05, 0x00, 0x00, 0x00, 0x00};
        basic_json_reflection parsed;
        reflection_bson_parser parser(min_doc);
        const bool ok = parser.parse(parsed);
        const std::string got = ok ? rjson::reflection_serializer(parsed).str() : std::string("<failed>");
        const std::string want = json::from_bson(min_doc).dump();
        if (!ok || got != want)
        {
            ++failures;
            std::printf("  [FAIL] %-32s want: %s got: %s err: %s\n", "minimal empty document", want.c_str(), got.c_str(), parser.error().c_str());
        }
        else
        {
            std::printf("  [ ok ] %-32s %s\n", "minimal empty document", want.c_str());
        }
    }
    // top level must be an object (library type_error 317 otherwise) — arrays nest inside
    check(json::object({{"a", json::array()}}), "empty array");
    check(json::object({{"a", json::array({1, "two", nullptr, false, 3.5})}}), "array of mixed types");
    check(json::object({{"a", json::array({json::array(), json::array({1})})}}), "nested arrays");
    check(json::object({{"inner", json::object({{"x", 1}, {"y", json::object({{"z", "deep"}})}})}}),
          "deeply nested objects");
    check(json::object({
              {"n", 42},
              {"s", "text"},
              {"arr", json::array({1, 2, json::object({{"in", true}})})},
              {"bin", json::binary_t(std::vector<std::uint8_t>{9, 8, 7})},
              {"nil", nullptr},
              {"f", 2.71828},
          }),
          "large mixed document");

    std::printf("\n[c] malformed input (safe failure, not library-identical codes)\n");
    check_negative({}, "empty input");
    // size=5 but the 5th byte is an element type with no name following -> EOF in cstring
    check_negative({0x05, 0x00, 0x00, 0x00, 0x01}, "element type with missing name");
    check_negative({0x0A, 0x00, 0x00, 0x00, 0x00}, "document size 0");
    // document: size=8 (int32) + 1 elem + terminator; element type 0xFF is unsupported
    check_negative({0x08, 0x00, 0x00, 0x00, 0xFF, 0x6B, 0x00, 0x00}, "unsupported element type 0xFF");
    check_negative({0x0C, 0x00, 0x00, 0x00, 0x01, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "truncated double payload");
    // size declares 12 but only 8 bytes follow
    check_negative({0x0C, 0x00, 0x00, 0x00, 0x0A, 0x6B, 0x00, 0x00}, "document size mismatch");
    // element name unterminated (no 0x00 before EOF)
    check_negative({0x07, 0x00, 0x00, 0x00, 0x0A, 0x6B, 0x6B}, "unterminated element name");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "M4B-2 PROBE PASS" : "M4B-2 PROBE FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
