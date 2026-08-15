// Access-control probe: can unchecked() access_context reflect PRIVATE
// nested types/members from OUTSIDE the class?
//
// Context: basic_json::json_value and basic_json::data are private nested
// entities (json.hpp:456 / :4272). P2996's access_context governs how much
// reflection sees; libstdc++ 16 provides current() / unprivileged() /
// unchecked(). This probe verifies that unchecked() lets external reflection
// enumerate private members (the premise of M2: reflecting the real
// tagged-union internals instead of a mirror type).
//
// Build: g++-16 -std=c++26 -freflection -O2 -o probe_access probe_access.cpp
#include <cstdio>
#include <cstdint>
#include <string_view>

#include <meta>

class Outer
{
  private:
    union json_value
    {
        int* object;
        long long number_integer;
        double number_float;
    };
    json_value m_value;
    int m_type;

  public:
    // in-class, unprivileged: private members are invisible to reflection too
    static consteval std::size_t probe_members()
    {
        return std::meta::nonstatic_data_members_of(^^Outer, std::meta::access_context::unprivileged()).size();
    }
};

// external reflection with unchecked() -- bypasses access control
consteval std::size_t unchecked_member_count()
{
    return std::meta::nonstatic_data_members_of(^^Outer, std::meta::access_context::unchecked()).size();
}

consteval std::size_t unchecked_union_member_count()
{
    // transient-vector rule: subscript directly on the call
    constexpr auto m_value =
        std::meta::nonstatic_data_members_of(^^Outer, std::meta::access_context::unchecked())[0];
    using U = typename [: std::meta::type_of(m_value) :];
    return std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::unchecked()).size();
}

int main()
{
    std::printf("in-class unprivileged member count = %zu\n", Outer::probe_members());
    std::printf("external unchecked member count     = %zu\n", unchecked_member_count());
    std::printf("external unchecked union members    = %zu\n", unchecked_union_member_count());

    bool ok = (Outer::probe_members() == 0)      // private hidden under unprivileged
           && (unchecked_member_count() == 2)    // m_value + m_type
           && (unchecked_union_member_count() == 3); // object / number_integer / number_float
    std::printf(ok ? "ACCESS PROBE PASSED\n" : "ACCESS PROBE MISMATCH\n");
    return ok ? 0 : 1;
}
