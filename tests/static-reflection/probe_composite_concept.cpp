// probe_composite_concept.cpp — can a composite multi-condition check be born as
// a concept? Two forms: (A) wrap (concept==trait) and (B) expand (hand-restate).
// Verify both equal the library trait across type matrix (g++-16, -std=c++26).
//  (A) wrap:   concept == trait::value
//  (B) expand: concept restates the multi-condition semantics
// Verify BOTH equal the library trait across a matrix of integer types.
#include <cstdio>
#include <type_traits>
#include <limits>
#include <utility>
#include <tuple>

#include <nlohmann/detail/meta/type_traits.hpp>  // single_include NOT used to avoid redefinition
using namespace nlohmann::detail;
using Real = long;                       // library number_integer_t on this platform
using int_pair_t = std::pair<int,int>;
using tuple1_t   = std::tuple<int>;

// (A) wrap
template<typename R, typename C>
concept compatible_integer_wrap = is_compatible_integer_type<R, C>::value;

// (B) expand — best-effort restatement, uses library is_constructible
template<typename R, typename C>
concept compatible_integer_expand =
    std::is_integral_v<R> &&
    std::is_integral_v<C> &&
    !std::is_same_v<bool, C> &&
    nlohmann::detail::is_constructible<R, C>::value &&
    std::numeric_limits<C>::is_integer &&
    std::numeric_limits<R>::is_signed == std::numeric_limits<C>::is_signed;

template<typename R, typename C>
constexpr bool trait_v = is_compatible_integer_type<R, C>::value;

// consensus helper: trait/wrap/expand must agree and equal `want`
template<typename C, bool want>
bool consensus(const char* name){
    constexpr bool tv = trait_v<Real,C>;
    constexpr bool wr = compatible_integer_wrap<Real,C>;
    constexpr bool ex = compatible_integer_expand<Real,C>;
    static_assert(tv == want, "trait disagrees with expected");
    if (!(tv==wr && tv==ex)){
        std::printf("  [DISAGREE] %-18s trait=%d wrap=%d expand=%d (want %d)\n",
                    name,(int)tv,(int)wr,(int)ex,(int)want);
        return true;
    }
    return false;
}

int main(){
    int bad = 0;
    bad += consensus<int,                true >("int");
    bad += consensus<short,              true >("short");
    bad += consensus<long long,          true >("long long");
    bad += consensus<unsigned long,      false>("unsigned long");
    bad += consensus<unsigned int,       false>("unsigned int");
    bad += consensus<bool,               false>("bool");
    bad += consensus<double,             false>("double");
    bad += consensus<int_pair_t,         false>("pair<int,int>");
    bad += consensus<tuple1_t,           false>("tuple<int>");

    std::printf(bad ? "COMPOSITE CONCEPT: %d DISAGREEMENTS\n" : "COMPOSITE CONCEPT: wrap=expand=trait CONSISTENT\n", bad);
    return bad ? 1 : 0;
}
