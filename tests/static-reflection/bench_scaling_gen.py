#!/usr/bin/env python3
"""bench_scaling_gen.py — generate the scaling benchmark TU.

The hand-written bench_macro_vs_reflection.cpp is capped at 100 structs (100
explicit PERSON() lines plus generated USE_ALL_0..100 macros), which is not
enough to answer "does the reflection path blow up as the type count grows?".
This generator emits the same person-like struct N times, so N is unbounded and
the sweep can reach 1000+ without bloating the repository with 1000 struct
lines.

The struct shape matches bench_macro_vs_reflection.cpp exactly (five fields
including a std::string and a std::vector<std::string>), so results from the two
harnesses are comparable.

Usage:
    python3 bench_scaling_gen.py <N> <mode> <output.cpp>

    mode = macro   NLOHMANN_DEFINE_TYPE_INTRUSIVE per type (the shipped path)
    mode = refl2   struct [[=refl2::json_serializable{}]] per type (the
                   reflection catch-all, JSON_USE_REFLECTION defined first)

Notes:
  * JSON_USE_REFLECTION must precede every include, because the gate is read
    when nlohmann/json.hpp pulls in the reflection headers.
  * each type is actually serialized AND deserialized in main(), so the TU
    instantiates the full bidirectional path rather than merely declaring it.
"""

import sys

FIELDS = [
    "    std::string name;",
    "    int age;",
    "    double height;",
    "    std::vector<std::string> tags;",
    "    bool active;",
]
MACRO_FIELDS = "name, age, height, tags, active"


def generate(n: int, mode: str) -> str:
    if mode not in ("macro", "refl2"):
        raise SystemExit(f"unknown mode {mode!r} (expected 'macro' or 'refl2')")

    out = []
    if mode == "refl2":
        out.append("#define JSON_USE_REFLECTION")
    out += [
        "#include <nlohmann/json.hpp>",
        "#include <cstdio>",
        "#include <string>",
        "#include <vector>",
        "",
        "using json = nlohmann::json;",
        "",
    ]

    for i in range(n):
        if mode == "macro":
            out.append(f"struct person_{i}")
            out.append("{")
            out.append(f"    NLOHMANN_DEFINE_TYPE_INTRUSIVE(person_{i}, {MACRO_FIELDS})")
            out += FIELDS
            out.append("};")
        else:
            out.append(f"struct [[=refl2::json_serializable{{}}]] person_{i}")
            out.append("{")
            out += FIELDS
            out.append("};")

    to_json = "refl2::codec<false>::to_json(j, p)" if mode == "refl2" else "nlohmann::to_json(j, p)"
    from_json = (
        "refl2::codec<false>::from_json(j, p)" if mode == "refl2" else "nlohmann::from_json(j, p)"
    )

    out += ["", "static std::size_t total = 0;", "", "int main()", "{"]
    for i in range(n):
        out.append(
            f"    {{ person_{i} p{{}}; json j; {to_json}; total += j.dump().size(); {from_json}; }}"
        )
    out += ['    std::printf("%zu\\n", total);', "    return 0;", "}"]
    return "\n".join(out) + "\n"


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    n, mode, dest = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    with open(dest, "w", encoding="utf-8") as fh:
        fh.write(generate(n, mode))


if __name__ == "__main__":
    main()
