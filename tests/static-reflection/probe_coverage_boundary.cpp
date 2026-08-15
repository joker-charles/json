// probe_coverage_boundary.cpp — verify what the refl2 coverage-boundary
// categories (EVALUATION.md §2.1) actually DO today: trait classification
// for each deliberately-unsupported case, the silent-inheritance drop, and
// the std::optional guard (adl branch, not array-like).
//
// Negative cases (compile-time) are documented in EVALUATION.md §2.1 and
// verified separately: union / std::variant / optional<PlainStruct> hit the
// clean priority-0 static_assert; map<exotic-key,int> errors in refl2's
// key_string; vector<non-default-constructible> from_json errors via the
// element's priority-0 static_assert; value_type-less incompatible ranges
// die deep in nlohmann's range path (adl branch).
//
// Compile: g++-16 -std=c++26 -freflection -O0 -Itests/static-reflection -Iinclude \
//            -o /tmp/pcb tests/static-reflection/probe_coverage_boundary.cpp && /tmp/pcb
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "refl2_codec.hpp"
#include <meta>

using json = nlohmann::json;

// (1) inheritance: public base with members
struct Base { std::string base_name; int base_id{}; };
struct Derived : Base { std::string own; };

// (2) union
union U { int i; double d; };

// (3) std::variant
using V = std::variant<int, std::string>;

// (4) optional<plain struct> / optional<int>
struct Plain { int x{}; };
using OptPlain = std::optional<Plain>;
using OptInt   = std::optional<int>;

// (5) raw pointer
using Ptr = int*;

// (6) range with begin/end but NO value_type, with a public member
struct RangeNoVT { int a{}; int* begin() { return nullptr; } int* end() { return nullptr; } };

// (7) map with a non-string/non-arithmetic key
struct Key { int k{}; };
using MapKey = std::map<Key, int>;

// (8) non-default-constructible element
struct NoDefault { NoDefault(int) {} };
using VecNoDefault = std::vector<NoDefault>;

#define SHOW(name, T) \
    std::printf("%-24s adl_elig=%d refl=%d arr=%d obj=%d\n", name, \
        (int)refl2::adl_branch_eligible<json, T>, \
        (int)refl2::is_reflectable_struct<false, T>::value, \
        (int)refl2::is_array_like<T>::value, \
        (int)refl2::is_object_like<T>::value)

int main()
{
    std::printf("=== refl2 coverage-boundary classification (unprivileged) ===\n");
    SHOW("Derived (inherits Base)", Derived);
    SHOW("union U", U);
    SHOW("std::variant<int,string>", V);
    SHOW("std::optional<Plain>", OptPlain);
    SHOW("std::optional<int>", OptInt);
    SHOW("int*", Ptr);
    SHOW("RangeNoVT (begin/end, no value_type)", RangeNoVT);
    SHOW("std::map<Key,int>", MapKey);
    SHOW("std::vector<NoDefault>", VecNoDefault);

    std::printf("\n=== runtime: inheritance (silent base-member drop?) ===\n");
    Derived d;
    d.base_name = "base"; d.base_id = 7; d.own = "own";
    json j;
    refl2::codec<false>::to_json(j, d);
    std::printf("Derived serialized: %s\n", j.dump().c_str());

    std::printf("\n=== runtime: std::optional<int> via adl branch ===\n");
    OptInt oi = 5;
    json jo;
    refl2::codec<false>::to_json(jo, oi);
    std::printf("optional<int> serialized: %s\n", jo.dump().c_str());

    std::printf("\nPROBE COVERAGE DONE\n");
    return 0;
}
