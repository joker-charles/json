// probe_coverage_boundary.cpp — verify what the refl2 coverage-boundary
// categories (EVALUATION.md §2.1) actually DO today: trait classification
// for each deliberately-unsupported case, the inheritance behavior
// (base-class members are now serialized via the subobjects_of recursion),
// and the std::optional dedicated branch (never array-like).
//
// Negative cases (compile-time) are documented in EVALUATION.md §2.1 and
// verified separately: union / pointers hit the clean priority-0
// static_assert (std::variant since M7 has a dedicated codec branch — see
// probe_variant.cpp); map<exotic-key,int> errors in refl2's key_string;
// vector<non-default-constructible> from_json errors via the element's
// priority-0 static_assert; value_type-less incompatible ranges die deep in
// nlohmann's range path (adl branch); the inheritance guards are clean
// static_asserts: private and protected bases (distinct messages via
// is_private / is_protected), virtual bases (is_virtual — a shared virtual
// subobject would duplicate its members), and duplicate member names across
// the hierarchy. Bit-fields serialize normally; unnamed bit-fields are
// skipped (not subobjects); bit-field from_json assigns via get<M>().
//
// Compile: g++-16 -std=c++26 -freflection -O0 -Itests/static-reflection -Iinclude \
//            -o /tmp/pcb tests/static-reflection/probe_coverage_boundary.cpp && /tmp/pcb
// Note: every user type here is exercised through refl2::codec<false> directly
// (the codec's priority-1 branch checks is_reflectable_struct, NOT the
// catch-all eligibility) or via std::variant (M7 whitelist exception), so no
// [[=refl2::json_serializable{}]] type-level annotation is required.
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>
#include <meta>

using json = nlohmann::json;

// (1) inheritance: public base with members
struct Base
{
    std::string base_name;
    int base_id{};
};
struct Derived : Base
{
    std::string own;
};

// (2) union
union U
{
    int i;
    double d;
};

// (3) std::variant
using V = std::variant<int, std::string>;

// (4) optional<plain struct> / optional<int>
struct Plain
{
    int x{};
};
using OptPlain = std::optional<Plain>;
using OptInt   = std::optional<int>;

// (5) raw pointer
using Ptr = int*;

// (6) range with begin/end but NO value_type, with a public member
struct RangeNoVT
{
    int a{};
    int* begin()
    {
        return nullptr;
    } int* end()
    {
        return nullptr;
    }
};

// (7) map with a non-string/non-arithmetic key
struct Key
{
    int k{};
};
using MapKey = std::map<Key, int>;

// (8) non-default-constructible element
struct NoDefault
{
    NoDefault(int) {}
};
using VecNoDefault = std::vector<NoDefault>;

#define SHOW(name, T) \
    std::printf("%-24s adl_elig=%d refl=%d arr=%d obj=%d\n", name, \
                (int)refl2::detail::to_adl_branch_eligible_v<json, T>, \
                (int)refl2::detail::is_reflectable_struct<false, T>::value, \
                (int)refl2::detail::is_array_like<T>::value, \
                (int)refl2::detail::is_object_like<T>::value)

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

    std::printf("\n=== runtime: inheritance (base members now serialized) ===\n");
    Derived d;
    d.base_name = "base";
    d.base_id = 7;
    d.own = "own";
    json j;
    refl2::codec<false>::to_json(j, d);
    std::printf("Derived serialized: %s\n", j.dump().c_str());

    std::printf("\n=== runtime: std::optional (dedicated branch) ===\n");
    OptInt oi = 5;
    json jo;
    refl2::codec<false>::to_json(jo, oi);
    std::printf("optional<int> serialized: %s\n", jo.dump().c_str());
    OptPlain op{Plain{3}};
    json jp;
    refl2::codec<false>::to_json(jp, op);
    std::printf("optional<Plain> serialized: %s\n", jp.dump().c_str());

    std::printf("\n=== runtime: std::variant (dedicated M7 branch) ===\n");
    V v = 42;
    json jv;
    refl2::codec<false>::to_json(jv, v);
    std::printf("variant<int,string> serialized: %s\n", jv.dump().c_str());
    V v2 = std::string("hi");
    json jv2;
    refl2::codec<false>::to_json(jv2, v2);
    std::printf("variant<int,string> (string alt) serialized: %s\n", jv2.dump().c_str());
    json jv3 = {{"index", 1}, {"value", "back"}};
    V v3 = jv3.template get<V>();
    std::printf("variant round-trip: %s (index=%zu)\n", std::get<std::string>(v3).c_str(), v3.index());

    std::printf("\nPROBE COVERAGE DONE\n");
    return 0;
}
