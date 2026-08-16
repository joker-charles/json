// cf_pointer.cpp — negative compile test: a raw POINTER must be rejected by
// the refl2 codec (self-referential / pointer types have no JSON mapping).
// Compilation MUST fail with the priority-0 static_assert.
// EXPECT-DIAG: pointers/self-referential
#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>

using json = nlohmann::json;

int main()
{
    json j;
    int* p = nullptr;
    refl2::codec<false>::to_json(j, p); // must not compile
}
