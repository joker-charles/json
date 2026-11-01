// optin_conflicts.cpp — precedence matrix for the opt-in front-ends.
//
// The comparison question is not only "can a type serialize?", but "which
// serializer wins when several opt-in/customization mechanisms are present?".
// This fixture locks the answers for:
//
//   * annotation               (adl_serializer partial specialization)
//   * enable<T>                (forwarding adl_serializer specialization)
//   * namespace `using` only   (ADL, custom non-template wins)
//   * enable<T> + using        (forwarding specialization wins over ADL)
//   * annotation + enable<T>   (annotation specialization, no ambiguity)
//   * NLOHMANN_DEFINE_TYPE_*   (non-template ADL function)
//   * explicit adl_serializer<T, void> specialization
//
// The key asymmetry: annotation and enable<T> bypass a hand-written ADL
// serializer because adl_serializer is specialized; namespace `using` alone
// goes through ADL, where the non-template hand-written function wins.
//
// Build/run:
//   g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address,undefined \
//       -Iinclude -o optin_conflicts tests/static-reflection/optin_conflicts.cpp
//   ./optin_conflicts

#define JSON_USE_REFLECTION

#include <nlohmann/reflection_namespace.hpp>

#include <cstdio>
#include <string>

using json = nlohmann::json;

namespace ann_custom
{
struct [[=nlohmann::json_serializable{}]] type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}
} // namespace ann_custom

namespace en_custom
{
struct type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}
} // namespace en_custom

namespace using_only_custom
{
struct type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace using_only_custom

namespace both_custom
{
struct type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace both_custom

namespace ann_and_en
{
struct [[=nlohmann::json_serializable{}]] type
{
    int x{};
};

void to_json(json& j, const type&)
{
    j = 999;
}

void from_json(const json& j, type& t)
{
    t.x = j.get<int>();
}
} // namespace ann_and_en

namespace ann_macro
{
struct [[=nlohmann::json_serializable{}]] type
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(type, x)
} // namespace ann_macro

namespace en_macro
{
struct type
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(type, x)
} // namespace en_macro

namespace using_only_macro
{
struct type
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(type, x)

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace using_only_macro

namespace en_using_macro
{
struct type
{
    int x{};
    int y{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(type, x)

using nlohmann::reflection::to_json;
using nlohmann::reflection::from_json;
} // namespace en_using_macro

namespace explicit_case
{
struct type
{
    int x{};
};
} // namespace explicit_case

namespace nlohmann
{
template<>
struct adl_serializer<explicit_case::type, void>
{
    template<typename BasicJsonType>
    static void to_json(BasicJsonType& j, const explicit_case::type&)
    {
        j = 42;
    }

    template<typename BasicJsonType>
    static void from_json(const BasicJsonType& j, explicit_case::type& t)
    {
        t.x = j.template get<int>();
    }
};
} // namespace nlohmann

namespace nlohmann::reflection
{
template<> inline constexpr bool enable<en_custom::type> = true;
template<> inline constexpr bool enable<both_custom::type> = true;
template<> inline constexpr bool enable<ann_and_en::type> = true;
template<> inline constexpr bool enable<en_macro::type> = true;
template<> inline constexpr bool enable<en_using_macro::type> = true;
template<> inline constexpr bool enable<explicit_case::type> = true;
} // namespace nlohmann::reflection

static int failures = 0;

static void check(const char* label, bool ok)
{
    std::printf("  %-58s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok)
    {
        ++failures;
    }
}

int main()
{
    std::printf("optin_conflicts — which serializer wins\n\n");

    std::printf("[1] annotation + custom ADL: annotation bypasses ADL\n");
    const json j1 = ann_custom::type{1};
    check("annotation wins", j1.dump() == R"({"x":1})");
    check("annotation from_json", j1.get<ann_custom::type>().x == 1);

    std::printf("\n[2] enable<T> + custom ADL: enable bypasses ADL\n");
    const json j2 = en_custom::type{1};
    check("enable wins", j2.dump() == R"({"x":1})");
    check("enable from_json", j2.get<en_custom::type>().x == 1);

    std::printf("\n[3] namespace `using` only + custom ADL: custom wins\n");
    const json j3 = using_only_custom::type{1};
    check("custom wins", j3.dump() == "999");
    check("custom from_json", json(999).get<using_only_custom::type>().x == 999);

    std::printf("\n[4] enable + using + custom ADL: enable still wins\n");
    const json j4 = both_custom::type{1};
    check("enable wins over using + custom", j4.dump() == R"({"x":1})");

    std::printf("\n[5] annotation + enable + custom ADL: annotation wins, no ambiguity\n");
    const json j5 = ann_and_en::type{1};
    check("annotation wins", j5.dump() == R"({"x":1})");

    std::printf("\n[6] annotation + NLOHMANN_DEFINE_TYPE_* (macro lists x only)\n");
    const json j6 = ann_macro::type{1, 2};
    check("annotation serializes every member", j6.dump() == R"({"x":1,"y":2})");

    std::printf("\n[7] enable<T> + NLOHMANN_DEFINE_TYPE_* (macro lists x only)\n");
    const json j7 = en_macro::type{1, 2};
    check("enable serializes every member", j7.dump() == R"({"x":1,"y":2})");

    std::printf("\n[8] namespace `using` only + NLOHMANN_DEFINE_TYPE_*: macro wins\n");
    const json j8 = using_only_macro::type{1, 2};
    check("macro wins", j8.dump() == R"({"x":1})");

    std::printf("\n[9] enable + using + NLOHMANN_DEFINE_TYPE_*: enable wins\n");
    const json j9 = en_using_macro::type{1, 2};
    check("enable wins", j9.dump() == R"({"x":1,"y":2})");

    std::printf("\n[10] explicit adl_serializer<T, void> wins over enable<T>\n");
    const json j10 = explicit_case::type{1};
    check("explicit serializer wins", j10.dump() == "42");
    check("explicit serializer from_json", json(42).get<explicit_case::type>().x == 42);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
