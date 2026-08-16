// m4d2_iterators.cpp — differential test: the basic_json_reflection iterator
// machinery (M4D-2) vs the real library's iterators / lookup / modifiers.
//
// Covers (g++-16, -std=c++26 -freflection):
//   1. begin/end iteration over object, array, primitive, string, binary,
//      null (element values byte-identical; null has begin == end);
//   2. reverse iteration (rbegin/rend);
//   3. object key()/value() during iteration;
//   4. iterator arithmetic: +/-, std::distance, operator[](n) on arrays;
//   5. operator[] member access incl. the null -> container conversion and
//      the out-of-range array fill-up;
//   6. find / contains / count;
//   7. erase(iterator) (array first/middle/last, object via find) and
//      erase(first, last) — dump parity after each;
//   8. error paths: erase on null/discarded, key() on a non-object iterator,
//      cross-container iterator comparison — the same exception CATEGORY as
//      the library (invalid_iterator ~ std::runtime_error here).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m4d2_iterators m4d2_iterators.cpp && ./m4d2_iterators
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using lib_json = nlohmann::json;
using rjson::basic_json_reflection;

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

std::string rdump(const basic_json_reflection& j)
{
    return rjson::reflection_serializer(j).str();
}

template<typename Fn>
std::string outcome(Fn&& f)
{
    try
    {
        f();
        return "ok";
    }
    catch (const nlohmann::detail::invalid_iterator&)
    {
        return "invalid_iterator";
    }
    catch (const nlohmann::detail::exception&)
    {
        return "nlohmann_other";
    }
    catch (const std::runtime_error&)
    {
        return "invalid_iterator"; // this study library maps iterator errors here
    }
    catch (const std::exception&)
    {
        return "std_other";
    }
}

// iterate and collect element dumps (values only, in order)
std::string iter_values(const lib_json& j)
{
    std::string s;
    for (const auto& v : j)
    {
        s += v.dump() + ";";
    }
    return s;
}
std::string iter_values(const basic_json_reflection& r)
{
    std::string s;
    for (const auto& v : r)
    {
        s += v.dump() + ";";
    }
    return s;
}

// iterate and collect key=value (object mode)
std::string iter_kv(const lib_json& j)
{
    std::string s;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        s += std::string(it.key()) + "=" + it.value().dump() + ";";
    }
    return s;
}
std::string iter_kv(const basic_json_reflection& r)
{
    std::string s;
    for (auto it = r.begin(); it != r.end(); ++it)
    {
        s += std::string(it.key()) + "=" + it.value().dump() + ";";
    }
    return s;
}

// reverse iteration
std::string iter_reverse(const lib_json& j)
{
    std::string s;
    for (auto it = j.rbegin(); it != j.rend(); ++it)
    {
        s += it->dump() + ";";
    }
    return s;
}
std::string iter_reverse(const basic_json_reflection& r)
{
    std::string s;
    for (auto it = r.rbegin(); it != r.rend(); ++it)
    {
        s += it->dump() + ";";
    }
    return s;
}

// run the same operation on both sides: on success the resulting dumps must
// be byte-identical; on failure BOTH must throw (the study library maps every
// error to std::runtime_error, so exact nlohmann category parity — type_error
// vs invalid_iterator — is not expressible here; ok-vs-threw parity is)
template<typename Fn>
void both(const char* what, const lib_json& src, Fn&& fn)
{
    lib_json real = src;
    basic_json_reflection refl;
    refl.assign_from(src);
    const std::string o1 = outcome([&] { fn(real); });
    const std::string o2 = outcome([&] { fn(refl); });
    ++checks;
    if ((o1 == "ok") != (o2 == "ok"))
    {
        report_fail(what, "outcome mismatch: real=" + o1 + " refl=" + o2);
        return;
    }
    if (o1 == "ok" && rdump(refl) != real.dump())
    {
        report_fail(what, "dump mismatch: real=" + real.dump() + " refl=" + rdump(refl));
    }
}

// ---------------------------------------------------------------------------
// 1/2/3/4. iteration basics
// ---------------------------------------------------------------------------
void check_iteration()
{
    std::printf("[1] iteration (values / keys / reverse)\n");

    const lib_json obj = {{"b", 2}, {"a", lib_json::array({1, 2})}, {"c", "x"}};
    basic_json_reflection robj;
    robj.assign_from(obj);
    CHECK(iter_values(robj) == iter_values(obj), "object value iteration");
    CHECK(iter_kv(robj) == iter_kv(obj), "object key/value iteration");
    CHECK(iter_reverse(robj) == iter_reverse(obj), "object reverse iteration");

    const lib_json arr = {1, "two", 3.5, nullptr, true};
    basic_json_reflection rarr;
    rarr.assign_from(arr);
    CHECK(iter_values(rarr) == iter_values(arr), "array value iteration");
    CHECK(iter_reverse(rarr) == iter_reverse(arr), "array reverse iteration");

    // primitives: one-element ranges (the reflection materializes the value)
    const lib_json prims[] = {lib_json(42), lib_json(3.5), lib_json(true),
                              lib_json("abc"), lib_json::binary({1, 2, 3})};
    for (const auto& p : prims)
    {
        basic_json_reflection rp;
        rp.assign_from(p);
        CHECK(iter_values(rp) == iter_values(p), "primitive iteration for %s", p.dump().c_str());
    }

    // null: begin == end (empty range)
    basic_json_reflection rnull;
    CHECK(rnull.begin() == rnull.end(), "null begin == end");
    CHECK(iter_values(rnull) == iter_values(lib_json(nullptr)), "null iteration empty");

    // const iteration path
    const basic_json_reflection& cobj = robj;
    CHECK(iter_values(cobj) == iter_values(obj), "const iteration");
    CHECK(iter_kv(cobj) == iter_kv(obj), "const key/value iteration");

    // iterator arithmetic: +/-, distance, operator[](n) on arrays
    {
        CHECK((rarr.end() - rarr.begin()) == 5, "iterator distance");
        auto it2 = rarr.begin() + 2;
        CHECK(it2->dump() == arr.at(2).dump(), "iterator + 2");
        CHECK((it2 - 2) == rarr.begin(), "iterator - 2");
        CHECK((2 + rarr.begin()) == it2, "scalar + iterator");
        CHECK(rarr.begin()[3].dump() == arr.at(3).dump(), "iterator operator[](3)");
        auto rb = rarr.rbegin();
        CHECK(rb->dump() == arr.back().dump(), "rbegin points at last element");
    }
}

// ---------------------------------------------------------------------------
// 5. operator[] member access
// ---------------------------------------------------------------------------
void check_operator_index()
{
    std::printf("[2] operator[] (incl. null conversion and fill-up)\n");

    // array access + out-of-range fill-up
    {
        lib_json real = lib_json::array({1, 2});
        basic_json_reflection refl;
        refl.assign_from(real);
        refl[3] = 9;
        real[3] = 9;
        CHECK(rdump(refl) == real.dump(), "operator[](idx) fill-up: %s vs %s",
              rdump(refl).c_str(), real.dump().c_str());
        CHECK(refl[1].dump() == real[1].dump(), "operator[](idx) existing element");
    }
    // null -> array conversion
    {
        lib_json real = lib_json(nullptr);
        basic_json_reflection refl;
        refl.assign_from(real);
        refl[0] = 42;
        real[0] = 42;
        CHECK(rdump(refl) == real.dump() && refl.is_array() && real.is_array(),
              "null -> array conversion: %s vs %s", rdump(refl).c_str(), real.dump().c_str());
    }
    // object access
    {
        lib_json real = lib_json::object({{"a", 1}});
        basic_json_reflection refl;
        refl.assign_from(real);
        refl["b"] = "x";
        real["b"] = "x";
        CHECK(rdump(refl) == real.dump(), "operator[](key): %s vs %s",
              rdump(refl).c_str(), real.dump().c_str());
    }
    // null -> object conversion
    {
        lib_json real = lib_json(nullptr);
        basic_json_reflection refl;
        refl.assign_from(real);
        refl["k"] = 7;
        real["k"] = 7;
        CHECK(rdump(refl) == real.dump() && refl.is_object() && real.is_object(),
              "null -> object conversion: %s vs %s", rdump(refl).c_str(), real.dump().c_str());
    }
    // wrong-type operator[] -> both throw
    both("operator[](idx) on object", lib_json::object({{"a", 1}}),
         [](auto& v) { static_cast<void>(v[std::size_t(0)]); });
    both("operator[](key) on array", lib_json::array({1}),
         [](auto& v) { static_cast<void>(v[std::string("k")]); });
}

// ---------------------------------------------------------------------------
// 6. find / contains / count
// ---------------------------------------------------------------------------
void check_lookup()
{
    std::printf("[3] find / contains / count\n");

    const lib_json obj = {{"a", 1}, {"b", 2}};
    basic_json_reflection refl;
    refl.assign_from(obj);

    const auto fit = refl.find("a");
    CHECK(fit != refl.end() && fit->dump() == "1", "find existing key");
    const auto mit = refl.find("nope");
    CHECK(mit == refl.end(), "find missing key");
    CHECK(mit == refl.find("nope"), "missing find == end");

    CHECK(refl.contains("b") && !refl.contains("nope"), "contains");
    CHECK(refl.count("a") == 1 && refl.count("nope") == 0, "count");

    // non-object: find/contains/count return end/false/0 (library semantics)
    basic_json_reflection rarr;
    rarr.assign_from(lib_json::array({1, 2}));
    CHECK(rarr.find("a") == rarr.end(), "array find == end");
    CHECK(!rarr.contains("a"), "array contains false");
    CHECK(rarr.count("a") == 0, "array count 0");

    // erase through find
    basic_json_reflection refl2;
    refl2.assign_from(obj);
    refl2.erase(refl2.find("a"));
    lib_json real2 = obj;
    real2.erase(real2.find("a"));
    CHECK(rdump(refl2) == real2.dump(), "erase(find) parity");
}

// ---------------------------------------------------------------------------
// 7/8. erase(iterator) / erase(first,last) + error paths
// ---------------------------------------------------------------------------
void check_erase_iter()
{
    std::printf("[4] erase(iterator) / erase(first,last)\n");

    both("erase(first array iterator)", lib_json::array({1, 2, 3}),
         [](auto& v) { static_cast<void>(v.erase(v.begin())); });
    both("erase(middle array iterator)", lib_json::array({1, 2, 3, 4}),
         [](auto& v) { static_cast<void>(v.erase(v.begin() + 2)); });
    both("erase(last array iterator)", lib_json::array({1, 2, 3}),
         [](auto& v) { static_cast<void>(v.erase(v.end() - 1)); });
    both("erase(range [1,3))", lib_json::array({1, 2, 3, 4, 5}),
         [](auto& v) { static_cast<void>(v.erase(v.begin() + 1, v.begin() + 3)); });
    both("erase(whole range)", lib_json::array({1, 2, 3}),
         [](auto& v) { static_cast<void>(v.erase(v.begin(), v.end())); });
    both("erase(iterator) on object via find", lib_json::object({{"a", 1}, {"b", 2}, {"c", 3}}),
         [](auto& v) { static_cast<void>(v.erase(v.find(std::string("b")))); });
    both("erase(object range [a,b))", lib_json::object({{"a", 1}, {"b", 2}, {"c", 3}}),
         [](auto& v) { static_cast<void>(v.erase(v.find(std::string("a")), v.find(std::string("c")))); });

    // erasing a primitive resets to null (library semantics)
    both("erase(iterator) on number", lib_json(42),
         [](auto& v) { static_cast<void>(v.erase(v.begin())); });
    both("erase(iterator) on string", lib_json("abc"),
         [](auto& v) { static_cast<void>(v.erase(v.begin())); });

    // error paths
    both("erase on null", lib_json(nullptr),
         [](auto& v) { static_cast<void>(v.erase(v.begin())); });
    both("erase on discarded", lib_json(lib_json::value_t::discarded),
         [](auto& v) { static_cast<void>(v.erase(v.begin())); });
    both("key() on array iterator", lib_json::array({1, 2}),
         [](auto& v) { static_cast<void>(v.begin().key()); });
    both("object iterator offset", lib_json::object({{"a", 1}}),
         [](auto& v) { static_cast<void>(v.begin() + 1); });

    // cross-container comparison throws on both sides (iterators of different
    // containers cannot be compared — the generic-lambda harness cannot
    // express this, so both sides are checked explicitly)
    {
        const lib_json a1 = lib_json::array({1});
        const lib_json a2 = lib_json::array({2});
        const bool real_threw = outcome([&] { static_cast<void>(a1.begin() == a2.begin()); }) != "ok";
        basic_json_reflection b1;
        basic_json_reflection b2;
        b1.assign_from(a1);
        b2.assign_from(a2);
        const bool refl_threw = outcome([&] { static_cast<void>(b1.begin() == b2.begin()); }) != "ok";
        CHECK(real_threw && refl_threw, "cross-container compare throws on both sides");
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    check_iteration();
    check_operator_index();
    check_lookup();
    check_erase_iter();

    std::printf("\nM4D-2 ITERATOR DIFF TEST %s (%zu checks)\n", ok ? "PASSED" : "FAILED", checks);
    return ok ? 0 : 1;
}
