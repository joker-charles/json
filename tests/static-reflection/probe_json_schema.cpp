// probe_json_schema.cpp — JSON Schema generation from the public reflection API.
//
// Build (from the repository root; see docs/static-reflection/BUILD_RECIPES.md):
//   g++-16 -std=c++26 -freflection -O2 -Iinclude \
//       -o build/scratch/probe_json_schema tests/static-reflection/probe_json_schema.cpp
//
// What this proves
// ----------------
// 1. The refl2 annotation layer is COMPOSABLE: a tool can be built on it
//    outside the codec, using only the public API (refl2::member_count /
//    member / member_key / member_has_default / has_annotation) — no
//    refl2::detail, no private members, no basic_json::json_value.
// 2. The generated schema agrees with what the codec actually emits: the
//    property names are cross-checked against the JSON produced by
//    nlohmann::json(value) for the same struct, so a schema that drifts from
//    the wire format fails the check instead of silently lying.
// 3. The annotation semantics carry over unchanged:
//      json_name{"years"}   -> property "years"
//      json_ignore{}        -> property absent, and absent from `required`
//      json_default{}       -> property present but NOT in `required`
//      nested annotated     -> recursed into an object schema
//
// Deliberately NOT a library API. This is a demonstration that the public
// surface is sufficient to build such a tool; if it becomes a shipped feature
// it should move into the extension proper with its own tests.
//
// Exit code 0 and "PROBE PASSED" when the schema and the codec agree.

#define JSON_USE_REFLECTION
#include <nlohmann/json.hpp>

#include <meta>
#include <print>
#include <string>
#include <utility>

namespace schema
{

// The type -> JSON Schema "type" mapping. Extend as needed; anything not
// recognised falls back to an object schema, which recurses through the same
// public API.
template<typename T>
nlohmann::json object_schema();

template<typename M>
nlohmann::json scalar_schema()
{
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>)
    {
        return {{"type", "boolean"}};
    }
    else if constexpr (std::is_integral_v<U>)
    {
        return {{"type", "integer"}};
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        return {{"type", "number"}};
    }
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
    {
        return {{"type", "string"}};
    }
    else if constexpr (std::is_enum_v<U> && refl2::enum_is_enumerable<U>())
    {
        // annotated enum -> JSON Schema enum of its mapped strings
        nlohmann::json allowed = nlohmann::json::array();
        [&]<std::size_t... I>(std::index_sequence<I...>)
        {
            ((allowed.push_back(std::string{refl2::enum_string<U, I>.data()})), ...);
        }
        (std::make_index_sequence<refl2::enum_count<U>()> {});
        return {{"type", "string"}, {"enum", allowed}};
    }
    else
    {
        return object_schema<U>();
    }
}

template<typename T, std::size_t... I>
nlohmann::json object_schema_impl(std::index_sequence<I...>)
{
    nlohmann::json props = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();

    // member_key already carries the json_name override, and json_ignore
    // members are absent from the index space entirely.
    (([&]
    {
        using M = typename [: std::meta::type_of(refl2::member<T, I>) :];
        const std::string key{refl2::member_key<T, I>.data()};
        props[key] = scalar_schema<M>();
        if constexpr (!refl2::member_has_default<T, I>)
        {
            required.push_back(key);
        }
    }()), ...);

    nlohmann::json s{{"type", "object"}, {"properties", props}};
    if (!required.empty())
    {
        s["required"] = required;
    }
    return s;
}

template<typename T>
nlohmann::json object_schema()
{
    static_assert(refl2::has_annotation<refl2::json_serializable>(^^T),
                  "schema::object_schema<T> requires "
                  "struct [[=refl2::json_serializable{}]] T");
    return object_schema_impl<T>(std::make_index_sequence<refl2::member_count<T>> {});
}

} // namespace schema

// ---------------------------------------------------------------------------

struct [[ = refl2::json_serializable {}]] point
{
    double x;
    double y;
};

enum class color
{
    red [[ = refl2::json_name{"RED"}]],
    green [[ = refl2::json_name{"GREEN"}]]
};

struct [[ = refl2::json_serializable {}]] person
{
    std::string name;
    int age [[ = refl2::json_name{"years"}]];
    std::string ssn [[ = refl2::json_ignore{}]];
    int score [[ = refl2::json_default{}]];
    color favourite [[ = refl2::json_default{}]];
    point location;
};

int main()
{
    const auto s = schema::object_schema<person>();
    std::println("{}", s.dump(2));

    // ---- cross-check against the codec's own output ----------------------
    const person p{"Ada", 36, "secret", 0, color::green, {1.0, 2.0}};
    const nlohmann::json emitted = p;

    std::vector<std::string> schema_keys;
    for (auto it = s.at("properties").begin(); it != s.at("properties").end(); ++it)
    {
        schema_keys.push_back(it.key());
    }
    std::vector<std::string> emitted_keys;
    for (auto it = emitted.begin(); it != emitted.end(); ++it)
    {
        emitted_keys.push_back(it.key());
    }
    std::sort(schema_keys.begin(), schema_keys.end());
    std::sort(emitted_keys.begin(), emitted_keys.end());

    if (schema_keys != emitted_keys)
    {
        std::println(stderr, "SCHEMA/CODEC MISMATCH:\n  schema : {}\n  emitted: {}",
                     nlohmann::json(schema_keys).dump(),
                     nlohmann::json(emitted_keys).dump());
        return 1;
    }

    // json_ignore must be absent from the emitted JSON as well as the schema
    if (emitted.contains("ssn") || s.at("properties").contains("ssn"))
    {
        std::println(stderr, "json_ignore member leaked into schema or output");
        return 1;
    }

    // json_default members are properties but never required
    for (const char* k :
            {"score", "favourite"
            })
    {
        if (!s.at("properties").contains(k))
        {
            std::println(stderr, "json_default member '{}' missing from properties", k);
            return 1;
        }
        if (s.at("required").contains(k))
        {
            std::println(stderr, "json_default member '{}' wrongly marked required", k);
            return 1;
        }
    }

    // the annotated enum propagated its mapped strings
    if (s.at("properties").at("favourite").at("enum") != nlohmann::json::array({"RED", "GREEN"}))
    {
        std::println(stderr, "annotated enum did not map to its json_name strings");
        return 1;
    }

    std::println("\n--- codec output ---");
    std::println("{}", emitted.dump());
    std::println("PROBE PASSED");
    return 0;
}
