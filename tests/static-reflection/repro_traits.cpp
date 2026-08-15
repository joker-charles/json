// repro_traits.cpp — empirical check for the A-class claim in M4_ASSESSMENT:
// can P2996 express "is this type one of OUR specific types" (the only
// category reflection might replace in type_traits.hpp)?
//
// We test reflection-based recognition of:
//   (1) is_basic_json<T>      — is T the same as nlohmann::json
//   (2) is_specialization_of  — is T a specialization of some template
//
// Caveat: this probe evaluates whether the approach COMPILES and returns the
// right value. It is a demonstration of feasibility, NOT a recommendation to
// adopt it in the library (see M4_ASSESSMENT §3).
//
// Build: g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude
#include <cstdio>
#include <string_view>
#include <type_traits>

#include <nlohmann/json.hpp>
#include <meta>

using json = nlohmann::json;

template<class T, class U> struct probe_pair {};

// ---- (1) reflection-based is_same_type on a concrete type ----
// This is the trivial core; std::meta::is_same_type exists in <meta>.
// Demonstrates that "is T exactly our type" (A-class) is directly expressible.
static_assert(std::meta::is_same_type(^^json, ^^json));
static_assert(!std::meta::is_same_type(^^json, ^^int));
static_assert(!std::meta::is_same_type(^^json, ^^probe_pair<int, long>));

// Recognise "T is an instance of template P" by template-name identity.
// Feature probe for is_specialization_of — this uses only compile-time-OK
// operations (no transient-vector binding).
template<class T>
consteval bool template_has_name(std::string_view expected)
{
    // identifier_of the template directly on the call (no local binding)
    return std::meta::identifier_of(std::meta::template_of(^^T)) == expected;
}

int main()
{
    std::printf("is_same_type(json,json)          = %d (expect 1)\n", (int)std::meta::is_same_type(^^json, ^^json));
    std::printf("is_same_type(json,int)           = %d (expect 0)\n", (int)std::meta::is_same_type(^^json, ^^int));
    std::printf("is_same_type(json,probe)         = %d (expect 0)\n", (int)std::meta::is_same_type(^^json, ^^probe_pair<int, long>));

    constexpr bool nm = template_has_name<probe_pair<int, long>>("probe_pair");
    std::printf("template-of name recognition     = %d (expect 1)\n", (int)nm);

    // NOTE: a true is_specialization_of<Primary,T> via reflection needs
    // template_of(^^T) == ^^Primary, which requires template-info equality.
    // template_arguments_of returns a transient vector (cannot bind to a local
    // constexpr), so the round-trip substitute() route is not directly usable
    // here — this is precisely the C-class friction the assessment predicts.

    bool ok = std::meta::is_same_type(^^json, ^^json)
           && !std::meta::is_same_type(^^json, ^^int)
           && !std::meta::is_same_type(^^json, ^^probe_pair<int, long>)
           && nm;
    std::printf(ok ? "TRAITS REFLECTION REPRO PASSED\n" : "TRAITS REFLECTION REPRO MISMATCH\n");
    return ok ? 0 : 1;
}
