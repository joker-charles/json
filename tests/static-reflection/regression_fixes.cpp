// regression_fixes.cpp — regression coverage for the defects found while
// reviewing the unmerged reflection work (2026-08). Every case below failed,
// aborted, or produced different bytes before its fix.
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -I include -o regression_fixes regression_fixes.cpp && ./regression_fixes
#include <nlohmann/reflection_binary.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using nlohmann::json;
using rjson::basic_json_reflection;

static int failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++failures;                                        \
            std::printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);\
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
        }                                                      \
    } while (false)

static std::string dump_of(const basic_json_reflection& r)
{
    rjson::reflection_serializer ser(r);
    return ser.str();
}

static std::vector<std::uint8_t> hex_to_bytes(const char* h)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; h[i] != '\0' && h[i + 1] != '\0'; i += 2)
    {
        out.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(h + i, 2).c_str(), nullptr, 16)));
    }
    return out;
}

// [1] iterator dereference must survive std::reverse_iterator's internal copy
//     (previously: reference into the copied iterator's own scratch -> ASan
//     stack-use-after-return)
static void test_reverse_iteration()
{
    std::printf("[1] reverse iteration over primitives/strings\n");
    for (const json& v :
            {
                json("hello"), json(42), json(3.5), json(true)
            })
    {
        basic_json_reflection r;
        r.assign_from(v);
        std::string got;
        for (auto it = r.rbegin(); it != r.rend(); ++it)
        {
            got += it->dump();
        }
        CHECK(got == v.dump(), "reverse dump %s != %s", got.c_str(), v.dump().c_str());
    }
}

// [2] erase(end()) on a primitive must throw instead of silently destroying
//     the value (upstream: invalid_iterator.205)
static void test_erase_end_guard()
{
    std::printf("[2] erase(end()) guard\n");
    basic_json_reflection r;
    r.assign_from(json("keep me"));
    bool threw = false;
    try
    {
        r.erase(r.end());
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw, "erase(end()) did not throw");
    CHECK(std::strcmp(r.type_name(), "string") == 0, "value was destroyed: %s", r.type_name());
}

// [3] JSON float/string output must be byte-identical to the library
//     (previously: std::to_chars style/digit drift incl. bare integer tokens,
//     and per-BYTE \\u00XX escaping of multi-byte UTF-8)
static void test_serializer_parity()
{
    std::printf("[3] serializer byte parity (floats + strings)\n");
    for (const double d :
            {
                1e300, 1e15, 1e16, 0.0001, 2e-4, 900719925474099.25,
                1.2345678901234568e17, 6.5535e20, 3.0, -0.0, 1.5
            })
    {
        const json v = d;
        basic_json_reflection r;
        r.assign_from(v);
        CHECK(dump_of(r) == v.dump(), "float %.17g: %s != %s", d, dump_of(r).c_str(), v.dump().c_str());
    }
    for (const char* s :
            {"é€中😀", "plain", "a\"b\\c", "\x01\x1f"
            })
    {
        const json v = s;
        for (const bool ascii :
                {
                    false, true
                })
        {
            basic_json_reflection r;
            r.assign_from(v);
            rjson::reflection_serializer ser(r, ascii);
            CHECK(ser.str() == v.dump(-1, ' ', ascii, nlohmann::detail::error_handler_t::strict),
                  "string %s (ascii=%d): %s != %s", s, static_cast<int>(ascii), ser.str().c_str(),
                  v.dump(-1, ' ', ascii).c_str());
        }
    }
    // invalid UTF-8 must throw type_error.316 like the library (strict default)
    bool threw = false;
    try
    {
        basic_json_reflection r;
        r.assign_from(json(std::string("\xff", 1)));
        static_cast<void>(dump_of(r));
    }
    catch (const nlohmann::detail::type_error&)
    {
        threw = true;
    }
    CHECK(threw, "invalid UTF-8 did not throw type_error.316");
}

// [4] CBOR non-finite values use the canonical half-float encodings
static void test_cbor_non_finite()
{
    std::printf("[4] CBOR NaN/inf half-float encodings\n");
    const struct
    {
        double value;
        const char* hex;
    } cases[] =
    {
        {std::numeric_limits<double>::quiet_NaN(), "f97e00"},
        {std::numeric_limits<double>::infinity(), "f97c00"},
        {-std::numeric_limits<double>::infinity(), "f9fc00"},
    };
    for (const auto& c : cases)
    {
        const json v = c.value;
        basic_json_reflection r;
        r.assign_from(v);
        rjson::reflection_cbor_serializer ser(r);
        std::string got;
        for (const auto b : ser.out)
        {
            char t[3];
            std::snprintf(t, sizeof t, "%02x", b);
            got += t;
        }
        CHECK(got == c.hex, "cbor %s != %s", got.c_str(), c.hex);
    }
}

// [5] UBJSON reader must read the library's own optimized output ($[ / ${)
static void test_ubjson_optimized_roundtrip()
{
    std::printf("[5] UBJSON optimized containers ($[ / ${)\n");
    const json cases[] =
    {
        json::array({json::array({1, 2}), json::array({3, 4})}),
        json::object({{"a", json::object({{"x", 1}})}}),
        json::array({json::array({1.5, 2.5}), json::array({3.5, 4.5})}),
    };
    for (const auto& c : cases)
    {
        const auto bytes = json::to_ubjson(c, true, true);
        basic_json_reflection parsed;
        rjson::reflection_ubjson_parser p(bytes);
        const bool ok = p.parse(parsed);
        CHECK(ok, "UBJSON optimized parse failed for %s: %s", c.dump().c_str(), p.error().c_str());
        if (ok)
        {
            CHECK(dump_of(parsed) == json::from_ubjson(bytes).dump(),
                  "UBJSON mismatch %s != %s", dump_of(parsed).c_str(), c.dump().c_str());
        }
    }
}

// [6] BSON duplicate keys are last-wins (upstream semantics)
static void test_bson_duplicate_keys()
{
    std::printf("[6] BSON duplicate keys (last wins)\n");
    const std::vector<std::uint8_t> doc =
    {
        0x13, 0, 0, 0, 0x10, 'a', 0, 1, 0, 0, 0, 0x10, 'a', 0, 2, 0, 0, 0, 0
    };
    basic_json_reflection parsed;
    rjson::reflection_bson_parser p(doc);
    const bool ok = p.parse(parsed);
    CHECK(ok, "BSON parse failed: %s", p.error().c_str());
    if (ok)
    {
        CHECK(dump_of(parsed) == json::from_bson(doc).dump(),
              "BSON duplicate keys: %s != %s", dump_of(parsed).c_str(), json::from_bson(doc).dump().c_str());
    }
}

// [7] malformed input must fail fast and must not pre-allocate the declared
//     container size (a 9-byte CBOR header used to reserve ~69 GB)
static void test_reader_hardening()
{
    std::printf("[7] reader hardening (strict EOF + bounded reserve)\n");
    {
        const auto bytes = hex_to_bytes("0102"); // trailing byte
        basic_json_reflection out;
        rjson::reflection_cbor_parser p(bytes);
        CHECK(!p.parse(out), "CBOR accepted trailing bytes");
    }
    {
        const auto bytes = hex_to_bytes("9b0000000100000000"); // array(2^32)
        basic_json_reflection out;
        rjson::reflection_cbor_parser p(bytes);
        CHECK(!p.parse(out), "CBOR accepted an impossible array size");
    }
    {
        const auto bytes = hex_to_bytes("5b234c0000010000000000"); // UBJSON #L 2^40
        basic_json_reflection out;
        rjson::reflection_ubjson_parser p(bytes);
        CHECK(!p.parse(out), "UBJSON accepted an impossible array size");
    }
}

int main()
{
    test_reverse_iteration();
    test_erase_end_guard();
    test_serializer_parity();
    test_cbor_non_finite();
    test_ubjson_optimized_roundtrip();
    test_bson_duplicate_keys();
    test_reader_hardening();

    if (failures == 0)
    {
        std::printf("\nREGRESSION PROBE PASS\n");
        return 0;
    }
    std::printf("\nREGRESSION PROBE FAILED (%d)\n", failures);
    return 1;
}
