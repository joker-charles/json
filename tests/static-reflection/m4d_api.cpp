// m4d_api.cpp — differential test: the completed basic_json_reflection API
// surface vs the real nlohmann/json library.
//
// Covers (g++-16, -std=c++26 -freflection):
//   1. value_t name / weight tables: kValueTNames / kValueTWeights and the
//      value_t_order / value_t_less helpers match value_t.hpp order[] /
//      operator<=> / operator< exactly on the full 10x10 type pair matrix
//      (incl. discarded -> unordered);
//   2. type_name() equals json::type_name() for every value_t;
//   3. size() / empty() match the library's element-count semantics;
//   4. at / erase / clear / swap behave identically on the happy path
//      (byte-identical dumps after the operation) and throw the same
//      exception CATEGORY on the error path (type_error vs out_of_range —
//      this study library uses plain std exceptions, mapped onto the
//      nlohmann exception taxonomy by tag);
//   5. all six comparison operators (== != < <= > >=) agree with the library
//      across a value matrix incl. NaN, cross-type numbers (signed/unsigned/
//      float), -0.0, large int64/uint64, and discarded values;
//   6. no leaks on repeated construct/destroy/swap/clear cycles (ASan).
//
// Build:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address \
//       -Isingle_include -Iinclude -o m4d_api m4d_api.cpp && ./m4d_api
#include <array>
#include <cstdio>
#include <cstring>     // strcmp
#include <limits>
#include <string>
#include <utility>     // index_sequence
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_json.hpp>

using lib_json = nlohmann::json;
using value_t  = nlohmann::detail::value_t;

constexpr auto kVTCount = rjson::kValueTInfos.size();

template<std::size_t... I>
consteval auto all_value_ts_impl(std::index_sequence<I...>)
{
    return std::array<value_t, sizeof...(I)>
    {
        static_cast<value_t>([: rjson::kValueTInfos[I] :])...
    };
}
constexpr auto kAllValueTs = all_value_ts_impl(std::make_index_sequence<kVTCount> {});
template<std::size_t... I>
consteval auto all_names_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)>
    {
        std::meta::identifier_of(rjson::kValueTInfos[I])...
    };
}
constexpr auto kAllNames = all_names_impl(std::make_index_sequence<kVTCount> {});

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

std::string rdump(const rjson::basic_json_reflection& j)
{
    return rjson::reflection_serializer(j).str();
}

// exception taxonomy tag: the study library throws plain std exceptions;
// nlohmann exceptions derive only from std::exception, so both sides are
// mapped onto a shared tag set (type_error ~ std::runtime_error,
// out_of_range ~ std::out_of_range).
template<typename Fn>
std::string outcome(Fn&& f)
{
    try
    {
        f();
        return "ok";
    }
    catch (const nlohmann::detail::out_of_range&)
    {
        return "out_of_range";
    }
    catch (const nlohmann::detail::type_error&)
    {
        return "type_error";
    }
    catch (const nlohmann::detail::exception&)
    {
        return "nlohmann_other";
    }
    catch (const std::out_of_range&)
    {
        return "out_of_range";
    }
    catch (const std::runtime_error&)
    {
        return "type_error";
    }
    catch (const std::exception&)
    {
        return "std_other";
    }
}

// run the same mutation on the real json and on the reflection value; the
// outcome tags must match, and on success the resulting dumps must be
// byte-identical
template<typename Fn>
void both_match(const char* what, const lib_json& src, Fn&& fn)
{
    lib_json real = src;
    rjson::basic_json_reflection refl;
    refl.assign_from(src);
    const std::string o1 = outcome([&] { fn(real); });
    const std::string o2 = outcome([&] { fn(refl); });
    ++checks;
    if (o1 != o2)
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
// 1. value_t name / weight tables vs the library's hand-written order[]
// ---------------------------------------------------------------------------
void check_value_t_tables()
{
    std::printf("[1] value_t name/weight tables vs order[]\n");

    // names must equal the reflected identifiers (table is generated from them)
    for (std::size_t i = 0; i < kVTCount; ++i)
    {
        CHECK(rjson::kValueTNames[i] == kAllNames[i],
              "kValueTNames[%zu] != reflected identifier", i);
    }

    // the full 10x10 pairwise differential of the ordering helpers against
    // the library's value_t::operator<=> / operator<
    for (std::size_t i = 0; i < kVTCount; ++i)
    {
        for (std::size_t j = 0; j < kVTCount; ++j)
        {
            const value_t a = kAllValueTs[i];
            const value_t b = kAllValueTs[j];
            const std::partial_ordering mine = rjson::refl_detail::value_t_order(a, b);
            const std::partial_ordering real = a <=> b; // ADL: nlohmann::detail
            ++checks;
            if (mine != real)
            {
                report_fail("value_t_order(" + std::string(rjson::kValueTNames[i]) + ", " +
                            std::string(rjson::kValueTNames[j]) + ") != library operator<=>");
            }
            ++checks;
            if (rjson::refl_detail::value_t_less(a, b) != (a < b))
            {
                report_fail("value_t_less(" + std::string(rjson::kValueTNames[i]) + ", " +
                            std::string(rjson::kValueTNames[j]) + ") != library operator<");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2/3. type_name / size / empty for every value_t
// ---------------------------------------------------------------------------
void check_type_and_size()
{
    std::printf("[2] type_name / size / empty for every value_t\n");
    for (std::size_t i = 0; i < kVTCount; ++i)
    {
        const value_t vt = kAllValueTs[i];
        lib_json real(vt);
        rjson::basic_json_reflection refl(vt);
        CHECK(std::strcmp(refl.type_name(), real.type_name()) == 0,
              "type_name(%s): refl=%s real=%s", std::string(rjson::kValueTNames[i]).c_str(),
              refl.type_name(), real.type_name());
        CHECK(refl.size() == real.size(), "size(%s): refl=%zu real=%zu",
              std::string(rjson::kValueTNames[i]).c_str(), refl.size(), real.size());
        CHECK(refl.empty() == real.empty(), "empty(%s) mismatch",
              std::string(rjson::kValueTNames[i]).c_str());
    }

    // container sizes
    const lib_json obj = {{"a", 1}, {"b", 2}, {"c", 3}};
    rjson::basic_json_reflection robj;
    robj.assign_from(obj);
    CHECK(robj.size() == obj.size() && robj.empty() == obj.empty() && !robj.empty(),
          "object size/empty mismatch");
    const lib_json arr = {1, 2, 3, 4};
    rjson::basic_json_reflection rarr;
    rarr.assign_from(arr);
    CHECK(rarr.size() == arr.size() && !rarr.empty(), "array size/empty mismatch");
    const lib_json empty_obj = lib_json::object();
    rjson::basic_json_reflection rempty;
    rempty.assign_from(empty_obj);
    CHECK(rempty.size() == 0 && rempty.empty(), "empty object size/empty mismatch");
}

// ---------------------------------------------------------------------------
// 4. at / erase / clear / swap
// ---------------------------------------------------------------------------
void check_at()
{
    std::printf("[3] at\n");
    const lib_json arr = {1, "two", 3.5, true};
    rjson::basic_json_reflection refl;
    refl.assign_from(arr);

    // happy path: values match element-wise (as dumps)

    // happy path: values match element-wise (as dumps)
    for (std::size_t i = 0; i < arr.size(); ++i)
    {
        CHECK(refl.at(i).dump() == arr.at(i).dump(), "at(%zu) value mismatch", i);
        const rjson::basic_json_reflection& crefl = refl;
        CHECK(crefl.at(i).dump() == arr.at(i).dump(), "const at(%zu) value mismatch", i);
    }

    // mutation through at() propagates into the stored container
    rjson::basic_json_reflection rm;
    rm.assign_from(arr);
    rm.at(1) = 99;
    lib_json real_mut = arr;
    real_mut.at(1) = 99;
    CHECK(rdump(rm) == real_mut.dump(), "at() mutation not visible in dump");

    // out-of-range array index -> both throw out_of_range
    both_match("at(out-of-range idx)", arr, [](auto& v) { static_cast<void>(v.at(5)); });
    // wrong type (object value used as array) -> both throw type_error
    both_match("at(idx) on object", lib_json{{"k", 1}}, [](auto& v) { static_cast<void>(v.at(0)); });
    both_match("at(idx) on string", lib_json("abc"), [](auto& v) { static_cast<void>(v.at(0)); });

    // object key access (nested object needs the double-brace form:
    // {"b", {"nested", true}} would build an ARRAY ["nested", true])
    const lib_json obj = {{"a", 1}, {"b", {{"nested", true}}}};
    rjson::basic_json_reflection robj;
    robj.assign_from(obj);
    CHECK(robj.at("a").dump() == obj.at("a").dump(), "at(key) value mismatch");
    CHECK(robj.at("b").at("nested").dump() == obj.at("b").at("nested").dump(),
          "nested at() value mismatch");
    both_match("at(missing key)", obj, [](auto& v) { static_cast<void>(v.at("nope")); });
    both_match("at(key) on array", arr, [](auto& v) { static_cast<void>(v.at("nope")); });
}

void check_erase()
{
    std::printf("[4] erase\n");
    // object key form
    const lib_json obj = {{"a", 1}, {"b", 2}, {"c", 3}};
    {
        lib_json real = obj;
        rjson::basic_json_reflection refl;
        refl.assign_from(obj);
        const std::size_t c1 = real.erase(std::string("b"));
        const std::size_t c2 = refl.erase(std::string("b"));
        CHECK(c1 == c2, "erase(key) count mismatch: real=%zu refl=%zu", c1, c2);
        CHECK(rdump(refl) == real.dump(), "erase(key) dump mismatch");
    }
    // missing key -> 0 erased, no throw
    both_match("erase(missing key)", obj, [](auto& v) { static_cast<void>(v.erase(std::string("nope"))); });
    // wrong type -> both throw type_error
    both_match("erase(key) on array", lib_json{1, 2}, [](auto& v) { static_cast<void>(v.erase(std::string("k"))); });

    // array index form
    const lib_json arr = {1, 2, 3, 4};
    both_match("erase(idx)", arr, [](auto& v) { v.erase(std::size_t(1)); });
    both_match("erase(first idx)", arr, [](auto& v) { v.erase(std::size_t(0)); });
    both_match("erase(last idx)", arr, [](auto& v) { v.erase(std::size_t(3)); });
    both_match("erase(out-of-range idx)", arr, [](auto& v) { v.erase(std::size_t(7)); });
    both_match("erase(idx) on object", obj, [](auto& v) { v.erase(std::size_t(0)); });
}

void check_clear()
{
    std::printf("[5] clear\n");
    const std::vector<lib_json> samples = {
        lib_json{{"a", 1}, {"b", {1, 2}}},
        lib_json{1, "two", 3.5},
        lib_json("some string"),
        lib_json::binary({1, 2, 3}, 7),
        lib_json(true),
        lib_json(42),
        lib_json(std::uint64_t(18446744073709551615ull)),
        lib_json(3.25),
        lib_json(nullptr),
        lib_json(value_t::discarded),
    };
    for (const auto& s : samples)
    {
        lib_json real = s;
        rjson::basic_json_reflection refl;
        refl.assign_from(s);
        real.clear();
        refl.clear();
        ++checks;
        if (rdump(refl) != real.dump() || std::strcmp(refl.type_name(), real.type_name()) != 0)
        {
            report_fail("clear() mismatch for " + real.dump() + ": after=" + rdump(refl));
        }
    }
}

void check_swap()
{
    std::printf("[6] swap\n");
    const lib_json a_src = {{"k", 1}};
    const lib_json b_src = {1, 2, 3};

    // member swap + ADL friend swap
    rjson::basic_json_reflection ra;
    ra.assign_from(a_src);
    rjson::basic_json_reflection rb;
    rb.assign_from(b_src);
    ra.swap(rb);
    CHECK(rdump(ra) == b_src.dump() && rdump(rb) == a_src.dump(), "member swap dump mismatch");
    swap(ra, rb); // ADL finds the friend
    CHECK(rdump(ra) == a_src.dump() && rdump(rb) == b_src.dump(), "ADL swap dump mismatch");

    // swap(array_t&) / swap(object_t&) / swap(string_t&) / swap(binary_t&)
    {
        rjson::basic_json_reflection rarr;
        rarr.assign_from(b_src);
        lib_json::array_t other{7, 8};
        rarr.swap(other);
        const lib_json want_arr = lib_json::array({7, 8});
        CHECK(rdump(rarr) == want_arr.dump() && other == lib_json::array_t({1, 2, 3}),
              "swap(array_t&) mismatch");
    }
    {
        rjson::basic_json_reflection robj;
        robj.assign_from(a_src);
        lib_json::object_t other{{"z", 9}};
        robj.swap(other);
        const lib_json want_obj = lib_json::object({{"z", 9}});
        const lib_json::object_t want_other{{"k", 1}};
        CHECK(rdump(robj) == want_obj.dump() && other == want_other,
              "swap(object_t&) mismatch");
    }
    {
        rjson::basic_json_reflection rstr;
        rstr.assign_from(lib_json("abc"));
        lib_json::string_t other = "xyz";
        rstr.swap(other);
        CHECK(rdump(rstr) == lib_json("xyz").dump() && other == "abc", "swap(string_t&) mismatch");
    }
    {
        rjson::basic_json_reflection rbin;
        rbin.assign_from(lib_json::binary({1, 2, 3}, 5));
        lib_json::binary_t other({4}, 9);
        rbin.swap(other);
        CHECK(rdump(rbin) == lib_json::binary({4}, 9).dump(), "swap(binary_t&) mismatch");
        lib_json::binary_t::container_type cont{6, 7};
        rbin.swap(cont);
        const lib_json want_bin = lib_json::binary({6, 7}, 9);
        CHECK(rdump(rbin) == want_bin.dump() && cont == lib_json::binary_t::container_type({4}),
              "swap(container_type&) mismatch");
    }
    // wrong-type swaps -> both throw type_error
    both_match("swap(array_t&) on object", a_src, [](auto& v) {
        lib_json::array_t tmp;
        v.swap(tmp);
    });
    both_match("swap(object_t&) on array", b_src, [](auto& v) {
        lib_json::object_t tmp;
        v.swap(tmp);
    });
    both_match("swap(string_t&) on array", b_src, [](auto& v) {
        lib_json::string_t tmp;
        v.swap(tmp);
    });
    both_match("swap(binary_t&) on array", b_src, [](auto& v) {
        lib_json::binary_t tmp;
        v.swap(tmp);
    });
}

// ---------------------------------------------------------------------------
// 5. comparison operators over a value matrix
// ---------------------------------------------------------------------------
void check_compare()
{
    std::printf("[7] comparison operators over value matrix\n");
    const std::vector<lib_json> vals = {
        lib_json(nullptr),
        lib_json(true),
        lib_json(false),
        lib_json(0), lib_json(1), lib_json(-1), lib_json(42), lib_json(-42),
        lib_json(0u), lib_json(1u),
        lib_json((std::numeric_limits<lib_json::number_unsigned_t>::max)()),
        lib_json((std::numeric_limits<lib_json::number_integer_t>::max)()),
        lib_json((std::numeric_limits<lib_json::number_integer_t>::min)()),
        lib_json(0.0), lib_json(-0.0), lib_json(1.5), lib_json(-1.5), lib_json(3.14),
        lib_json(1e100), lib_json(-1e100),
        lib_json(std::numeric_limits<double>::quiet_NaN()),
        lib_json(std::numeric_limits<double>::infinity()),
        lib_json(-std::numeric_limits<double>::infinity()),
        lib_json(""), lib_json("a"), lib_json("abc"),
        lib_json::array(), lib_json::array({1, 2}), lib_json::array({"x", lib_json::array({true})}),
        lib_json::object(), lib_json::object({{"a", 1}, {"b", "z"}}),
        lib_json::binary({1, 2, 3}),
        lib_json(value_t::discarded),
    };

    // dump parity first: every matrix value must serialize identically
    for (std::size_t i = 0; i < vals.size(); ++i)
    {
        rjson::basic_json_reflection refl;
        refl.assign_from(vals[i]);
        ++checks;
        if (rdump(refl) != vals[i].dump())
        {
            report_fail("dump parity for matrix value " + std::to_string(i) +
                        " (" + vals[i].dump() + "): refl=" + rdump(refl));
        }
    }

    // full pairwise differential of all six operators
    for (std::size_t i = 0; i < vals.size(); ++i)
    {
        rjson::basic_json_reflection ra;
        ra.assign_from(vals[i]);
        for (std::size_t j = 0; j < vals.size(); ++j)
        {
            rjson::basic_json_reflection rb;
            rb.assign_from(vals[j]);
            const bool r_eq = (vals[i] == vals[j]);
            const bool m_eq = (ra == rb);
            const bool r_ne = (vals[i] != vals[j]);
            const bool m_ne = (ra != rb);
            const bool r_lt = (vals[i] < vals[j]);
            const bool m_lt = (ra < rb);
            const bool r_le = (vals[i] <= vals[j]);
            const bool m_le = (ra <= rb);
            const bool r_gt = (vals[i] > vals[j]);
            const bool m_gt = (ra > rb);
            const bool r_ge = (vals[i] >= vals[j]);
            const bool m_ge = (ra >= rb);
            ++checks;
            if (r_eq != m_eq || r_ne != m_ne || r_lt != m_lt || r_le != m_le ||
                r_gt != m_gt || r_ge != m_ge)
            {
                report_fail("comparison mismatch (" + std::to_string(i) + "," +
                            std::to_string(j) + ") real=" + vals[i].dump() + " vs " +
                            vals[j].dump() + " refl=" + rdump(ra) + " vs " + rdump(rb) +
                            " [== " + std::to_string(r_eq) + "/" + std::to_string(m_eq) +
                            " != " + std::to_string(r_ne) + "/" + std::to_string(m_ne) +
                            " < " + std::to_string(r_lt) + "/" + std::to_string(m_lt) +
                            " <= " + std::to_string(r_le) + "/" + std::to_string(m_le) +
                            " > " + std::to_string(r_gt) + "/" + std::to_string(m_gt) +
                            " >= " + std::to_string(r_ge) + "/" + std::to_string(m_ge) + "]");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 6. leak cycles under ASan
// ---------------------------------------------------------------------------
void check_leaks()
{
    std::printf("[8] construct/destroy/swap/clear cycles (ASan)\n");
    const lib_json samples[] = {
        lib_json{{"a", {1, 2}}, {"b", "x"}},
        lib_json{1, "two", lib_json::array({true})},
        lib_json::binary({1, 2, 3}, 4),
        lib_json(3.5),
        lib_json(value_t::discarded),
    };
    for (std::size_t round = 0; round < 200; ++round)
    {
        for (const auto& s : samples)
        {
            rjson::basic_json_reflection a;
            a.assign_from(s);
            rjson::basic_json_reflection b;
            b.assign_from(s);
            a.swap(b);
            a.clear();
            a.assign_from(samples[round % 5]);
        }
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: abort() must not hide progress
    check_value_t_tables();
    check_type_and_size();
    check_at();
    check_erase();
    check_clear();
    check_swap();
    check_compare();
    check_leaks();

    std::printf("\nM4D API DIFF TEST %s (%zu checks)\n", ok ? "PASSED" : "FAILED", checks);
    return ok ? 0 : 1;
}
