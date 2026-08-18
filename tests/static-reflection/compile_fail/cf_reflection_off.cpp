// cf_reflection_off.cpp — negative compile test: a DIRECT include of
// reflection_to_json.hpp without JSON_USE_REFLECTION must fail with the
// opt-in #error (the reflection catch-all is a behavior change; the include
// chain {to,from}_json.hpp checks the same macro, and this #error keeps
// direct includes honest — UPSTREAM_INTEGRATION_PLAN.md §2.2).
// EXPECT-DIAG: requires JSON_USE_REFLECTION
#include <nlohmann/reflection_to_json.hpp>

int main()
{
    return 0;
}
