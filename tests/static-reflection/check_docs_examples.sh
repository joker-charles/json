#!/usr/bin/env bash
# check_docs_examples.sh — keep the reflection feature page honest.
#
# docs/mkdocs/docs/features/reflection.md carries executable claims about the
# standalone extension (<nlohmann/reflection.hpp>). Nothing compiled them, so
# the page could drift from the code silently. This harness closes that loop
# for the claims the page actually makes:
#
#   1. the annotated struct's documented output — the ```cpp block is extracted
#      from the page VERBATIM, wrapped in the fixtures the page shows, and
#      compiled and run; the JSON it prints must match what the page documents;
#   2. per-type opt-in — an UNANNOTATED type must not become serializable;
#   3. the actionable static_assert — a member with no serialization path must
#      fail to compile with the message the page quotes.
#
# (2) and (3) are negative claims, so they are checked by compiling and
# requiring failure with the documented diagnostic, not by running.
#
# Usage:  GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
# Requires: g++-16 (-std=c++26 -freflection) and python3.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
PAGE="${REPO}/docs/mkdocs/docs/features/reflection.md"
OUT_DIR="${OUT_DIR:-${REPO}/build/scratch/docs-check}"
INCS=(-std=c++26 -freflection -I"${REPO}/include")

mkdir -p "${OUT_DIR}"
pass=0
fail=0
check() { # name, rc
    if [ "$2" -eq 0 ]; then
        printf '  [PASS] %s\n' "$1"
        pass=$((pass + 1))
    else
        printf '  [FAIL] %s\n' "$1"
        fail=$((fail + 1))
    fi
}

[ -f "${PAGE}" ] || { echo "error: feature page not found: ${PAGE}" >&2; exit 1; }
command -v "${GXX}" >/dev/null 2>&1 || { echo "error: compiler '${GXX}' not found" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "error: python3 required" >&2; exit 1; }

echo "== features/reflection.md: documented example =="
python3 - "$PAGE" "$OUT_DIR" "$REPO" "$GXX" <<'PY'
import re
import subprocess
import sys

page_path, out_dir, repo, gxx = sys.argv[1:5]
page = open(page_path, encoding="utf-8").read()

# The page's exemplar: the annotated struct plus its documented serialization.
blocks = re.findall(r"```cpp\n(.*?)```", page, re.S)
exemplar = [b for b in blocks if "json_serializable" in b and "double x" in b]
if not exemplar:
    print("  [FAIL] could not find the annotated-struct example block")
    sys.exit(1)
if "[[=refl2::" in page:
    print("  [FAIL] the page still documents the legacy 'refl2::' namespace")
    sys.exit(1)

harness = """#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

""" + exemplar[0] + """
int main()
{
    const nlohmann::json j = point{1.0, 2.0};
    std::printf("%s\\n", j.dump().c_str());
    const auto p = j.get<point>();
    std::printf("%.1f %.1f\\n", p.x, p.y);
}
"""

src = f"{out_dir}/doc_example.cpp"
open(src, "w", encoding="utf-8").write(harness)
build = subprocess.run(
    [gxx, *("-std=c++26", "-freflection"), f"-I{repo}/include", "-o", f"{out_dir}/doc_example", src],
    capture_output=True, text=True)
if build.returncode != 0:
    print("  [FAIL] the page's cpp block does not compile")
    for line in build.stderr.splitlines()[:5]:
        print("        " + line)
    sys.exit(1)

run = subprocess.run([f"{out_dir}/doc_example"], capture_output=True, text=True)
if run.returncode != 0:
    print("  [FAIL] the page's cpp block compiles but does not run")
    sys.exit(1)

expected = '{"x":1.0,"y":2.0}'
lines = run.stdout.splitlines()
if not lines or lines[0] != expected:
    print("  [FAIL] the documented output is not what the code produces")
    print(f"        produced  : {lines[0] if lines else '<no output>'}")
    print(f"        documented: {expected}")
    sys.exit(1)

print("  [PASS] the annotated-struct example compiles and prints the documented JSON")
sys.exit(0)
PY
check "documented example" $?

echo "== per-type opt-in (negative claim) =="
cat > "${OUT_DIR}/unannotated.cpp" <<'EOF'
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>
struct plain { int x{}; };
int main() { nlohmann::json j = plain{1}; }
EOF
if "${GXX}" "${INCS[@]}" -fsyntax-only "${OUT_DIR}/unannotated.cpp" 2>"${OUT_DIR}/unannotated.err"; then
    check "an unannotated type stays non-serializable" 1
else
    check "an unannotated type stays non-serializable" 0
fi

echo "== actionable static_assert (negative claim) =="
cat > "${OUT_DIR}/bad_member.cpp" <<'EOF'
#include <nlohmann/json.hpp>
#include <nlohmann/reflection.hpp>
struct not_serializable { int v{}; };
struct [[=nlohmann::json_serializable{}]] holder { not_serializable bad; };
int main() { nlohmann::json j = holder{{1}}; j.dump(); }
EOF
if "${GXX}" "${INCS[@]}" -fsyntax-only "${OUT_DIR}/bad_member.cpp" 2>"${OUT_DIR}/bad_member.err"; then
    check "a member with no serialization path is a compile error" 1
else
    if grep -q "json_serializable member is not JSON-serializable" "${OUT_DIR}/bad_member.err"; then
        check "a member with no serialization path is a compile error with the documented message" 0
    else
        check "a member with no serialization path is a compile error with the documented message" 1
        sed 's/^/        /' "${OUT_DIR}/bad_member.err" | head -5
    fi
fi

printf '\nDOCS EXAMPLE CHECK: %d passed, %d failed\n' "$pass" "$fail"
exit "$fail"
