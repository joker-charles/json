// cf_unannotated_struct.cpp — negative compile test: WITH the opt-in macro
// defined, a reflectable struct WITHOUT the [[=refl2::json_serializable{}]]
// type-level annotation must NOT serialize (per-type opt-in,
// UPSTREAM_INTEGRATION_PLAN.md §2.2). The catch-all does not participate for
// it, so the error is the main library's "conversion from ... to non-scalar
// type" — byte-identical to a build without reflection at all.
// EXPECT-DIAG: non-scalar type
#define JSON_USE_REFLECTION 1  // opt-in gate (UPSTREAM_INTEGRATION_PLAN.md §2.2)
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Point  // NOTE: intentionally NOT annotated
{
    double x;
    double y;
};

int main()
{
    json j = Point{1.0, 2.0}; // must not compile
}
