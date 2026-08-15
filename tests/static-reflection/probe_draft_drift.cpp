// probe_draft_drift.cpp — quantify drift of SEMANTIC-RESTATEMENT concepts vs
// real traits: exactly 1 drift (vector<uint8_t> binary) shows why precise probe
// targets matter.
// Build: g++-16 -std=c++26 -freflection -O0 -Isingle_include -Iinclude
// Compare the USER's draft layered concepts (semantic restatement) vs PRECISE
// concepts (exact probe targets) against the real traits. Quantify drift.
#include <cstdio>
#include <type_traits>
#include <vector>
#include <map>
#include <string>
#include <set>
#include <cstdint>
#include <typeinfo>
#include <cstring>

#include <nlohmann/json.hpp>
using json = nlohmann::json; using B = json;

// ===== user's draft (semantic restatement) =====
template<typename T> concept has_value_type_d  = requires { typename T::value_type; };
template<typename T> concept has_mapped_type_d = requires { typename T::mapped_type; };
template<typename T> concept has_begin_end_d   = requires(T& t) { t.begin(); t.end(); };

template<typename T>
concept array_like_draft =
    has_value_type_d<T> && has_begin_end_d<T> &&
    std::convertible_to<typename T::value_type, json>;

template<typename T>
concept object_like_draft =
    has_mapped_type_d<T> && has_begin_end_d<T> &&
    std::convertible_to<typename T::mapped_type, json>;

template<typename T>
concept string_like_draft =
    std::convertible_to<T, std::string> ||
    (has_begin_end_d<T> &&
     std::same_as<std::remove_cv_t<typename T::value_type>, char>);

template<typename T>
concept binary_like_draft =
    has_value_type_d<T> &&
    std::same_as<std::remove_cv_t<typename T::value_type>, std::uint8_t>;

// ===== truth =====
template<typename B, typename T> static constexpr bool t_array  = nlohmann::detail::is_compatible_array_type<B,T>::value;
template<typename B, typename T> static constexpr bool t_object = nlohmann::detail::is_compatible_object_type<B,T>::value;
template<typename B, typename T> static constexpr bool t_string = nlohmann::detail::is_compatible_string_type<B,T>::value;
template<typename B, typename T> static constexpr bool t_binary = nlohmann::detail::is_compatible_binary_type<B,T>::value;

int main(){
    using Vint = std::vector<int>;
    using Vstr = std::vector<std::string>;
    using Vjson = std::vector<json>;
    using Str = std::string;
    using Map = std::map<std::string,int>;
    using Set = std::set<int>;
    using Vbyte = std::vector<std::uint8_t>;
    const char* names[] = { "vector<int>","vector<string>","vector<json>","string","map<string,int>","set<int>","vector<uint8_t>" };
    const int N = 7;
    int drift = 0;
    auto row = [&](const char* dim, auto f){
        // print agreement summary for this dimension across the 7 types
        int agree = 0;
        for (int i=0;i<N;++i){
            const char* n = names[i];
            if (dim[0]=='a' && f.template val<0>(i)) {}
        }
    };
    // simpler: check per dimension per type with a macro-free table
    struct Case { const char* name; bool ta,to,ts,tb; bool da,do_,ds,db; };
    // Build cases manually:
    std::printf("%-16s | array(obj/str/bin) | draft array(obj/str/bin)\n","type");
    auto show = [&](const char* n,
                    bool ta,bool to_,bool ts,bool tb,
                    bool da,bool do_,bool ds,bool db){
        bool agree = (ta==da)&&(to_==do_)&&(ts==ds)&&(tb==db);
        if (!agree){ ++drift; std::printf("  [DRIFT] %-14s trait=(%d%d%d%d) draft=(%d%d%d%d)\n", n,
            ta,to_,ts,tb, da,do_,ds,db); }
    };
    show(names[0], t_array<B,Vint>, t_object<B,Vint>, t_string<B,Vint>, t_binary<B,Vint>,
         array_like_draft<Vint>, object_like_draft<Vint>, string_like_draft<Vint>, binary_like_draft<Vint>);
    show(names[1], t_array<B,Vstr>, t_object<B,Vstr>, t_string<B,Vstr>, t_binary<B,Vstr>,
         array_like_draft<Vstr>, object_like_draft<Vstr>, string_like_draft<Vstr>, binary_like_draft<Vstr>);
    show(names[2], t_array<B,Vjson>, t_object<B,Vjson>, t_string<B,Vjson>, t_binary<B,Vjson>,
         array_like_draft<Vjson>, object_like_draft<Vjson>, string_like_draft<Vjson>, binary_like_draft<Vjson>);
    show(names[3], t_array<B,Str>, t_object<B,Str>, t_string<B,Str>, t_binary<B,Str>,
         array_like_draft<Str>, object_like_draft<Str>, string_like_draft<Str>, binary_like_draft<Str>);
    show(names[4], t_array<B,Map>, t_object<B,Map>, t_string<B,Map>, t_binary<B,Map>,
         array_like_draft<Map>, object_like_draft<Map>, string_like_draft<Map>, binary_like_draft<Map>);
    show(names[5], t_array<B,Set>, t_object<B,Set>, t_string<B,Set>, t_binary<B,Set>,
         array_like_draft<Set>, object_like_draft<Set>, string_like_draft<Set>, binary_like_draft<Set>);
    show(names[6], t_array<B,Vbyte>, t_object<B,Vbyte>, t_string<B,Vbyte>, t_binary<B,Vbyte>,
         array_like_draft<Vbyte>, object_like_draft<Vbyte>, string_like_draft<Vbyte>, binary_like_draft<Vbyte>);

    std::printf(drift ? "DRAFT CONCEPTS: %d DRIFTS vs traits\n" : "DRAFT CONCEPTS == traits\n", drift);
    std::fflush(stdout);
    return drift ? 1 : 0;
}
