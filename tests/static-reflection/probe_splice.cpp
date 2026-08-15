// Probe: external read/write of PRIVATE members via splice, using info
// obtained through access_context::unchecked().
//
// Validates the premise of M2 for the REAL library shape: basic_json's
// json_value/data are private nested types; this probe shows that reflection
// code OUTSIDE the class can both enumerate them (see probe_access.cpp) and
// actually read/write their members through splices.
//
// Build: g++-16 -std=c++26 -freflection -O2 -o probe_splice probe_splice.cpp
#include <cstdio>

#include <meta>

class Outer
{
  private:
    int secret = 42;

  public:
    int get() const
    {
        return secret;
    }
};

consteval auto private_member()
{
    // transient-vector rule: subscript directly on the call
    return std::meta::nonstatic_data_members_of(^^Outer, std::meta::access_context::unchecked())[0];
}

int main()
{
    constexpr auto m = private_member();
    Outer o;

    int v = o.[:m:];              // external read of a private member
    std::printf("external read via splice = %d\n", v);
    o.[:m:] = 7;                  // external write of a private member
    std::printf("after external write     = %d\n", o.get());

    bool ok = (v == 42) && (o.get() == 7);
    std::printf(ok ? "PRIVATE-SPLICE PROBE PASSED\n" : "PRIVATE-SPLICE PROBE MISMATCH\n");
    return ok ? 0 : 1;
}
