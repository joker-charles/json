#!/usr/bin/env bash
# measure_optin.sh — same-core codegen check for the two opt-in front-ends.
#
# Generates N types with identical members, once behind the annotation and once
# behind enable<T>, compiles both at -O2, and compares the object sizes. The
# expected result is identical text/data/bss because both front-ends call the
# same detail::reflection_flat::to_object/from_object core.
#
# Usage:
#   GXX=g++-16 N=40 bash tests/static-reflection/measure_optin.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
N="${N:-40}"
OUT="${OUT:-$REPO/build/optin-measure}"
mkdir -p "$OUT"

python3 - "$OUT" "$N" <<'PY'
import sys
from pathlib import Path

out = Path(sys.argv[1])
n = int(sys.argv[2])

ann = [
    "#define JSON_USE_REFLECTION",
    "#include <nlohmann/reflection.hpp>",
    "using nlohmann::json;",
]
en = [
    "#define JSON_USE_REFLECTION",
    "#include <nlohmann/reflection_namespace.hpp>",
    "using nlohmann::json;",
]

for i in range(n):
    ann.append(f"struct [[=nlohmann::json_serializable{{}}]] S{i} {{ int a{{}}; double b{{}}; }};")
    en.append(f"struct S{i} {{ int a{{}}; double b{{}}; }};")

en.append("namespace nlohmann::reflection {")
en.extend(f"template<> inline constexpr bool enable<S{i}> = true;" for i in range(n))
en.append("}")

for i in range(n):
    ann.append(f"json f{i}() {{ return S{i}{{1, 2.0}}; }}")
    en.append(f"json f{i}() {{ return S{i}{{1, 2.0}}; }}")

(out / "codegen_ann.cpp").write_text("\n".join(ann) + "\n", encoding="utf-8")
(out / "codegen_en.cpp").write_text("\n".join(en) + "\n", encoding="utf-8")
PY

"$GXX" -std=c++26 -freflection -O2 -I"$REPO/include" -c "$OUT/codegen_ann.cpp" -o "$OUT/codegen_ann.o"
"$GXX" -std=c++26 -freflection -O2 -I"$REPO/include" -c "$OUT/codegen_en.cpp" -o "$OUT/codegen_en.o"

printf 'N=%s object sizes (annotation vs enable<T>):\n' "$N"
size "$OUT/codegen_ann.o" "$OUT/codegen_en.o"

ann_size="$(size "$OUT/codegen_ann.o" | awk 'NR==2 {print $1, $2, $3}')"
en_size="$(size "$OUT/codegen_en.o" | awk 'NR==2 {print $1, $2, $3}')"
if [ "$ann_size" = "$en_size" ]; then
    printf 'IDENTICAL text/data/bss: %s\n' "$ann_size"
    exit 0
fi
printf 'DIFFERENT: annotation=[%s] enable=[%s]\n' "$ann_size" "$en_size"
exit 1
