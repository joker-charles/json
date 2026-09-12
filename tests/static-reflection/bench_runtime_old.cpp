// bench_runtime_old.cpp — "before" variant of bench_runtime.cpp for the
// refl2 codec runtime baseline (EVALUATION.md §2.2's "refl2 v2 (old codec)"
// column).
//
// Identical to bench_runtime.cpp except that the BENCH_ADL_REFLECTION mode
// includes refl2_codec_old.hpp (the pre-optimization codec extracted from
// commit 62290f3b) instead of the current refl2_codec.hpp. Build with the
// same flags as the v2 mode:
//   g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
//     -o /tmp/brt_old bench_runtime_old.cpp
// Driver: tests/static-reflection/runtime_measure.sh (binary brt_old_v2).
//
// bench_runtime.cpp — serialize/deserialize throughput: macro vs refl v1 vs
// refl2 v2 (the ADL-aware recursive reflection serializer).
//
// Closes the EVALUATION.md §2.2 runtime gap: §1.2 measured the concepts
// change's runtime; this bench measures the reflection layer's conversion
// cost (to_json = DOM build, from_json = DOM read) for the same data under
// the three serialization mechanisms. dump/parse are library code and
// byte-identical across modes; a round-trip timing (to_json + dump + parse +
// from_json) is included only as an end-to-end no-regression check.
//
// Modes (same flags as bench_macro_vs_reflection.cpp):
//   macro: g++-16 -std=c++26 -freflection -O2 -Iinclude \
//            -o /tmp/brt bench_runtime.cpp
//   v1:    ... -DBENCH_REFLECTION ...
//   v2:    ... -DBENCH_ADL_REFLECTION ...
//
// Env-tunable: BENCH_ITER (default 200000), BENCH_RUNS (default 7, median).
//
// Methodology: per direction, a warmup loop, then BENCH_RUNS timed runs of
// BENCH_ITER iterations; each iteration varies the input with the loop index
// (age += i%7, name += char) so the work is NOT loop-invariant and cannot be
// hoisted/eliminated at -O2; the per-iteration result feeds a printed sink
// counter. Reported value: median ns/op of the timed runs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef BENCH_REFLECTION
    #include <meta> // v1 serializer
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Types — conditional definitions per mode (same pattern as the compile bench)
// ---------------------------------------------------------------------------
#ifdef BENCH_ADL_REFLECTION
#define DEFINE_ADDRESS() struct Address \
    { \
        std::string street; \
        std::string city; \
        int zip{}; \
    };
#define DEFINE_FLAT() struct FlatPerson \
    { \
        std::string name; \
        int age{}; \
        double height{}; \
        std::vector<std::string> tags; \
        bool active{}; \
    };
#define DEFINE_NESTED() struct NestedPerson \
    { \
        std::string name; \
        int age{}; \
        Address home; \
        std::vector<Address> history; \
    };
#elif defined(BENCH_REFLECTION)
// v1: plain structs, flat only (v1 cannot serialize nested types)
#define DEFINE_ADDRESS()
#define DEFINE_FLAT() struct FlatPerson \
    { \
        std::string name; \
        int age{}; \
        double height{}; \
        std::vector<std::string> tags; \
        bool active{}; \
    };
#define DEFINE_NESTED()
#else
#define DEFINE_ADDRESS() struct Address \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Address, street, city, zip) \
        std::string street; \
        std::string city; \
        int zip{}; \
    };
#define DEFINE_FLAT() struct FlatPerson \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(FlatPerson, name, age, height, tags, active) \
        std::string name; \
        int age{}; \
        double height{}; \
        std::vector<std::string> tags; \
        bool active{}; \
    };
#define DEFINE_NESTED() struct NestedPerson \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(NestedPerson, name, age, home, history) \
        std::string name; \
        int age{}; \
        Address home; \
        std::vector<Address> history; \
    };
#endif

DEFINE_ADDRESS()
DEFINE_FLAT()
DEFINE_NESTED()
#undef DEFINE_ADDRESS
#undef DEFINE_FLAT
#undef DEFINE_NESTED

// ---------------------------------------------------------------------------
// The mode: a uniform static to_json/from_json interface
// ---------------------------------------------------------------------------
#ifdef BENCH_ADL_REFLECTION
#include "refl2_codec_old.hpp"
using Mode = refl2::codec<false>;
#define MODE_LABEL "refl2 v2"
#elif defined(BENCH_REFLECTION)
// the v1 naive serializer (EVALUATION.md §2.1) — flat structs only
namespace v1
{
template<typename BasicJsonType, typename T>
void to_json(BasicJsonType& j, const T& v)
{
    j = BasicJsonType::object();
    template for (constexpr auto m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())))
    {
        j[std::string(std::meta::identifier_of(m))] = v.[:m:];
    }
}
template<typename BasicJsonType, typename T>
void from_json(const BasicJsonType& j, T& v)
{
    template for (constexpr auto m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())))
    {
        using M = typename [: std::meta::type_of(m) :];
        v.[:m:] = j.at(std::string(std::meta::identifier_of(m))).template get<M>();
    }
}
} // namespace v1
struct mode_v1
{
    template<typename B, typename T>
    static void to_json(B& j, const T& v)
    {
        v1::to_json(j, v);
    }
    template<typename B, typename T>
    static void from_json(const B& j, T& v)
    {
        v1::from_json(j, v);
    }
};
using Mode = mode_v1;
#define MODE_LABEL "refl v1"
#else
struct mode_macro
{
    template<typename B, typename T>
    static void to_json(B& j, const T& v)
    {
        nlohmann::to_json(j, v);
    }
    template<typename B, typename T>
    static void from_json(const B& j, T& v)
    {
        nlohmann::from_json(j, v);
    }
};
using Mode = mode_macro;
#define MODE_LABEL "macro"
#endif

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
#ifndef BENCH_ITER
    #define BENCH_ITER 200000
#endif
#ifndef BENCH_RUNS
    #define BENCH_RUNS 7
#endif

static std::size_t g_sink = 0; // printed at the end; keeps work observable

// vary the input with the loop index so the work is not loop-invariant
template<typename T>
static void vary(T& v, std::size_t i)
{
    v.age += static_cast<int>(i % 7);
    v.name.push_back(static_cast<char>('a' + (i % 26)));
}

// median ns/op over BENCH_RUNS timed runs of BENCH_ITER iterations of one_op
template<typename Fn>
static double median_ns_per_op(Fn&& one_op)
{
    std::vector<double> samples;
    samples.reserve(BENCH_RUNS);
    // warmup (also forces one-time inits, e.g. refl2's static keys)
    for (std::size_t i = 0; i < BENCH_ITER / 16 + 1; ++i)
    {
        one_op(i);
    }
    for (int r = 0; r < BENCH_RUNS; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < BENCH_ITER; ++i)
        {
            one_op(i);
        }
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count()
                          / static_cast<double>(BENCH_ITER));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// data-dependent fingerprints so the sink provably depends on the work
static std::size_t flat_fp(const json& j)
{
    return j.at("name").get_ref<const std::string&>().size()
           + static_cast<std::size_t>(j.at("age").get<int>())
           + j.at("tags").size();
}
static std::size_t nested_fp(const json& j)
{
    return j.at("name").get_ref<const std::string&>().size()
           + static_cast<std::size_t>(j.at("age").get<int>())
           + j.at("history").size()
           + static_cast<std::size_t>(j.at("home").at("zip").get<int>());
}

template<typename T, typename Fp>
static double bench_serialize(const T& p, Fp fp)
{
    return median_ns_per_op([&](std::size_t i)
    {
        T v = p;
        vary(v, i);
        json j;
        Mode::to_json(j, v);
        g_sink += fp(j);
    });
}

template<typename T, typename Fp>
static double bench_deserialize(const T& p, Fp fp)
{
    // pre-build 8 varying sources; the loop reads j[i % 8] — the input varies
    // with i, so the from_json call cannot be hoisted or eliminated
    std::vector<json> srcs;
    srcs.reserve(8);
    for (std::size_t k = 0; k < 8; ++k)
    {
        T v = p;
        vary(v, k * 13);
        json j;
        Mode::to_json(j, v);
        srcs.push_back(std::move(j));
    }
    return median_ns_per_op([&](std::size_t i)
    {
        T out{};
        Mode::from_json(srcs[i % 8], out);
        g_sink += out.name.size() + static_cast<std::size_t>(out.age);
    });
}

template<typename T, typename Fp>
static double bench_round_trip(const T& p, Fp fp)
{
    return median_ns_per_op([&](std::size_t i)
    {
        T v = p;
        vary(v, i);
        json j;
        Mode::to_json(j, v);
        const std::string text = j.dump();
        json j2 = json::parse(text);
        T out{};
        Mode::from_json(j2, out);
        g_sink += fp(j2) + out.name.size();
    });
}

int main(int argc, char** argv)
{
    const int seed = argc > 1 ? std::atoi(argv[1]) : 12345;
    FlatPerson flat;
    flat.name = "person_" + std::to_string(seed);
    flat.age = seed % 90;
    flat.height = 1.5 + (seed % 100) / 100.0;
    flat.tags = {std::to_string(seed % 7), "x", "y"};
    flat.active = (seed % 2) != 0;

    std::printf("bench_runtime mode=%s iters=%d runs=%d seed=%d\n",
                MODE_LABEL, BENCH_ITER, BENCH_RUNS, seed);
    std::printf("  flat     serialize   %8.3f us/op\n", bench_serialize(flat, flat_fp) / 1000.0);
    std::printf("  flat     deserialize %8.3f us/op\n", bench_deserialize(flat, flat_fp) / 1000.0);
    std::printf("  flat     round-trip  %8.3f us/op\n", bench_round_trip(flat, flat_fp) / 1000.0);

#ifndef BENCH_REFLECTION
    NestedPerson nested;
    nested.name = "nested_" + std::to_string(seed);
    nested.age = seed % 80;
    nested.home = {"1 Main St " + std::to_string(seed % 100), "Springfield", 10000 + seed % 9000};
    nested.history =
    {
        {"2 Old Rd", "Shelbyville", 11111},
        {"3 New Ave", "Capital City", 22222}
    };
    std::printf("  nested   serialize   %8.3f us/op\n", bench_serialize(nested, nested_fp) / 1000.0);
    std::printf("  nested   deserialize %8.3f us/op\n", bench_deserialize(nested, nested_fp) / 1000.0);
    std::printf("  nested   round-trip  %8.3f us/op\n", bench_round_trip(nested, nested_fp) / 1000.0);
#else
    std::printf("  nested   (v1 cannot serialize nested types — skipped)\n");
#endif

    std::printf("  sink=%zu\n", g_sink);
    return 0;
}
