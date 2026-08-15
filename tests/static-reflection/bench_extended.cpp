// bench_extended.cpp — -O2 binary + runtime for the NEW codec paths
// (inheritance / std::optional / bit-fields), macro vs refl2.
//
// Closes the EVALUATION.md §2.2 gap: the existing bench only covers flat and
// shallow-nested person types, so the inheritance / optional / bit-field
// dispatch branches added to refl2_codec.hpp have no measured evidence that
// they (a) collapse like the flat path at -O2 (binary size) and (b) stay
// within the macro baseline's throughput (runtime).
//
// Modes:
//   macro: g++-16 -std=c++26 -freflection -O2 -Iinclude \
//            -o /tmp/bxe tests/static-reflection/bench_extended.cpp
//   refl2: ... -DBENCH_ADL_REFLECTION ...
//
// Macro-side counterparts (the honest baseline where one exists):
//   * inheritance: NLOHMANN_DEFINE_TYPE_INTRUSIVE(DerivedPerson, base_name,
//     base_id, mid, own) — listing base-class public members in the macro is
//     legal (the expansion is j["base_name"] = v.base_name).
//   * optional: Address gets NLOHMANN_DEFINE_TYPE_INTRUSIVE; OptionalPerson
//     lists its std::optional<Address> member (native nlohmann support).
//   * bit-fields: NO macro counterpart for from_json (get_to needs a T&, a
//     bit-field has no address — same limitation as refl2's ordinary path);
//     the baseline is a hand-written to_json/from_json using get<M>()
//     assignment, i.e. the "ideal macro" shape.
//
// Methodology (same as bench_runtime.cpp): warmup + BENCH_RUNS timed runs of
// BENCH_ITER iterations, median us/op; inputs vary with the loop index so
// -O2 cannot hoist/eliminate the work; results feed a printed sink counter.
// Both modes must print identical totals (parity = identical JSON).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef BENCH_ADL_REFLECTION
    #include "refl2_codec.hpp"
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
#ifdef BENCH_ADL_REFLECTION
    #define DEFINE_INHERITED() struct Base2 \
    { \
        std::string base_name; \
        int base_id{}; \
    }; \
    struct Mid2 : Base2 { std::string mid; }; \
    struct DerivedPerson : Mid2 { int own{}; };
    #define DEFINE_ADDRESS() struct Address \
    { \
        std::string street; \
        std::string city; \
        int zip{}; \
    };
    #define DEFINE_OPTIONAL() struct OptionalPerson \
    { \
        std::string name; \
        std::optional<Address> home; \
    };
    #define DEFINE_BITFIELD() struct BitFieldStruct \
    { \
        int a : 3; \
        int b : 5; \
        int c{}; \
    };
#else
    #define DEFINE_INHERITED() struct Base2 \
    { \
        std::string base_name; \
        int base_id{}; \
    }; \
    struct Mid2 : Base2 { std::string mid; }; \
    struct DerivedPerson : Mid2 \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(DerivedPerson, base_name, base_id, mid, own) \
        int own{}; \
    };
    #define DEFINE_ADDRESS() struct Address \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Address, street, city, zip) \
        std::string street; \
        std::string city; \
        int zip{}; \
    };
    #define DEFINE_OPTIONAL() struct OptionalPerson \
    { \
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(OptionalPerson, name, home) \
        std::string name; \
        std::optional<Address> home; \
    };
    #define DEFINE_BITFIELD() struct BitFieldStruct \
    { \
        int a : 3; \
        int b : 5; \
        int c{}; \
    }; \
    void to_json(json& j, const BitFieldStruct& v) \
    { \
        j = json{{"a", v.a}, {"b", v.b}, {"c", v.c}}; \
    } \
    void from_json(const json& j, BitFieldStruct& v) \
    { \
        v.a = j.at("a").get<int>(); \
        v.b = j.at("b").get<int>(); \
        v.c = j.at("c").get<int>(); \
    }
#endif

DEFINE_INHERITED()
DEFINE_ADDRESS()
DEFINE_OPTIONAL()
DEFINE_BITFIELD()
#undef DEFINE_INHERITED
#undef DEFINE_ADDRESS
#undef DEFINE_OPTIONAL
#undef DEFINE_BITFIELD

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------
#ifdef BENCH_ADL_REFLECTION
    using Mode = refl2::codec<false>;
    #define MODE_LABEL "refl2"
#else
    struct mode_macro
    {
        template<typename B, typename T>
        static void to_json(B& j, const T& v) { nlohmann::to_json(j, v); }
        template<typename B, typename T>
        static void from_json(const B& j, T& v) { nlohmann::from_json(j, v); }
    };
    using Mode = mode_macro;
    #define MODE_LABEL "macro"
#endif

// ---------------------------------------------------------------------------
// Harness (same anti-elimination design as bench_runtime.cpp)
// ---------------------------------------------------------------------------
#ifndef BENCH_ITER
    #define BENCH_ITER 200000
#endif
#ifndef BENCH_RUNS
    #define BENCH_RUNS 7
#endif

static std::size_t g_sink = 0;

template<typename Fn>
static double median_ns_per_op(Fn&& one_op)
{
    std::vector<double> samples;
    samples.reserve(BENCH_RUNS);
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

// vary and out_fp are per-type callbacks (no if-constexpr-with-members —
// GCC 16 does not discard the false branch, and requires() guards are not
// worth the risk here); serialize sink is j.size() (forces the DOM build).
template<typename T, typename VaryFn, typename OutFp>
static void bench_all(const char* label, const T& p, VaryFn vary, OutFp out_fp)
{
    // pre-built sources (i%8 polling) — deserialize input varies with i
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

    auto ser = median_ns_per_op([&](std::size_t i)
    {
        T v = p;
        vary(v, i);
        json j;
        Mode::to_json(j, v);
        g_sink += j.size();
    });
    auto des = median_ns_per_op([&](std::size_t i)
    {
        T out{};
        Mode::from_json(srcs[i % 8], out);
        g_sink += out_fp(out);
    });
    auto rt = median_ns_per_op([&](std::size_t i)
    {
        T v = p;
        vary(v, i);
        json j;
        Mode::to_json(j, v);
        const std::string text = j.dump();
        json j2 = json::parse(text);
        T out{};
        Mode::from_json(j2, out);
        g_sink += out_fp(out) + j2.size();
    });
    std::printf("  %-10s serialize   %8.3f us/op\n", label, ser / 1000.0);
    std::printf("  %-10s deserialize %8.3f us/op\n", label, des / 1000.0);
    std::printf("  %-10s round-trip  %8.3f us/op\n", label, rt / 1000.0);
}

int main(int argc, char** argv)
{
    const int seed = argc > 1 ? std::atoi(argv[1]) : 12345;

    DerivedPerson dp;
    dp.base_name = "base_" + std::to_string(seed);
    dp.base_id = seed % 90;
    dp.mid = "mid_" + std::to_string(seed);
    dp.own = seed % 7;

    OptionalPerson op;
    op.name = "opt_" + std::to_string(seed);
    op.home = Address{"1 Main", "Springfield", 10000 + seed % 9000};

    BitFieldStruct bf{};
    bf.a = seed % 7;
    bf.b = seed % 31;
    bf.c = seed % 100;

    std::printf("bench_extended mode=%s iters=%d runs=%d seed=%d\n",
                MODE_LABEL, BENCH_ITER, BENCH_RUNS, seed);

    bench_all("inherited", dp,
              [](DerivedPerson& v, std::size_t i)
              {
                  v.base_id += static_cast<int>(i % 5);
                  v.mid.push_back(static_cast<char>('a' + (i % 26)));
                  v.own += static_cast<int>(i % 7);
              },
              [](const DerivedPerson& v) { return v.own + v.base_id + v.mid.size(); });

    bench_all("optional", op,
              [](OptionalPerson& v, std::size_t i)
              {
                  v.name.push_back(static_cast<char>('a' + (i % 26)));
                  if (i % 2 == 0)
                  {
                      v.home.emplace(Address{"1 St", "Town", static_cast<int>(i % 1000)});
                  }
                  else
                  {
                      v.home.reset();
                  }
              },
              [](const OptionalPerson& v) { return v.name.size() + (v.home ? 1 : 0); });

    bench_all("bitfield", bf,
              [](BitFieldStruct& v, std::size_t i)
              {
                  v.a = static_cast<int>(i % 7);
                  v.b = static_cast<int>(i % 31);
                  v.c += static_cast<int>(i % 3);
              },
              [](const BitFieldStruct& v) { return v.a + v.b + v.c; });

    std::printf("  sink=%zu\n", g_sink);
    return 0;
}
