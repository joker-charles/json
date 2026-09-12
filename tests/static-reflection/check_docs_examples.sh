#!/usr/bin/env bash
# check_docs_examples.sh — keep the reflection feature page honest.
#
# docs/mkdocs/docs/features/reflection.md carries two fenced blocks that are
# executable claims about the extension:
#
#   * a ```cpp block defining `namespace schema` (the JSON Schema generator),
#   * a ```json block showing what that generator produces for `person`.
#
# Nothing compiled them, so the page could drift from the code silently — the
# documentation would keep asserting behaviour the library no longer had. This
# harness closes that loop:
#
#   1. probe_json_schema.cpp is compiled AND run; it must print PROBE PASSED
#      (it cross-checks the schema's keys against the codec's real output).
#   2. the ```cpp block is extracted from the page VERBATIM, wrapped in the
#      fixtures the page shows, compiled, and run.
#   3. its output is compared against the ```json block in the page, parsed
#      rather than string-compared so key order does not matter.
#
# A page edit that no longer compiles, or that documents output the code does
# not produce, fails here.
#
# Usage:  GXX=g++-16 bash tests/static-reflection/check_docs_examples.sh
# Requires: g++-16 (-std=c++26 -freflection) and python3.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
PAGE="${REPO}/docs/mkdocs/docs/features/reflection.md"
FIXTURE="${SCRIPT_DIR}/probe_json_schema.cpp"
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
[ -f "${FIXTURE}" ] || { echo "error: fixture not found: ${FIXTURE}" >&2; exit 1; }
command -v "${GXX}" >/dev/null 2>&1 || { echo "error: compiler '${GXX}' not found" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "error: python3 required" >&2; exit 1; }

echo "== probe_json_schema.cpp =="
if "${GXX}" "${INCS[@]}" -O2 -o "${OUT_DIR}/probe_json_schema" "${FIXTURE}" 2>"${OUT_DIR}/probe.err"; then
    if ! "${OUT_DIR}/probe_json_schema" >"${OUT_DIR}/probe.out" 2>&1; then
        # the fixture's own cross-check failed: its schema and the codec
        # disagree about the keys, which is the drift this guards against
        check "fixture self-check FAILED (schema keys diverge from the codec)" 1
        sed 's/^/        /' "${OUT_DIR}/probe.out" | head -5
    elif ! grep -q 'PROBE PASSED' "${OUT_DIR}/probe.out"; then
        check "fixture ran but did not report PROBE PASSED" 1
    else
        check "fixture compiles, runs and self-checks (schema keys == codec keys)" 0
    fi
else
    check "fixture compiles" 1
    sed 's/^/        /' "${OUT_DIR}/probe.err" | head -5
fi

echo "== features/reflection.md code block =="
python3 - "$PAGE" "$OUT_DIR" "$REPO" "$GXX" <<'PY'
import json
import re
import subprocess
import sys

page_path, out_dir, repo, gxx = sys.argv[1:5]
page = open(page_path, encoding="utf-8").read()

cpp_blocks = [b for b in re.findall(r"```cpp\n(.*?)```", page, re.S) if "namespace schema" in b]
json_blocks = [b for b in re.findall(r"```json\n(.*?)```", page, re.S) if '"properties"' in b]
if not cpp_blocks or not json_blocks:
    print("  [FAIL] could not find the schema code block and/or its output block")
    sys.exit(1)

# The page shows only the generator; the fixtures and main() below are the
# rest of the example, kept here so the extracted block stays verbatim.
harness = """#define JSON_USE_REFLECTION
#include <nlohmann/json.hpp>
#include <meta>
#include <print>
#include <string>
#include <utility>

""" + cpp_blocks[0] + """
struct [[=refl2::json_serializable{}]] point { double x; double y; };
struct [[=refl2::json_serializable{}]] person {
    std::string name;
    int age [[=refl2::json_name{"years"}]];
    std::string ssn [[=refl2::json_ignore{}]];
    int score [[=refl2::json_default{}]];
    point location;
};
int main() { std::println("{}", schema::object_schema<person>().dump(2)); }
"""

src = f"{out_dir}/doc_example.cpp"
open(src, "w", encoding="utf-8").write(harness)
build = subprocess.run(
    [gxx, "-std=c++26", "-freflection", f"-I{repo}/include",
     "-o", f"{out_dir}/doc_example", src],
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

# Compare parsed, not literal: nlohmann::json sorts object keys, and the page
# is free to present them in any order as long as the document is the same.
try:
    produced = json.loads(run.stdout)
    documented = json.loads(json_blocks[0])
except json.JSONDecodeError as exc:
    print(f"  [FAIL] could not parse produced or documented JSON: {exc}")
    sys.exit(1)

if produced != documented:
    print("  [FAIL] the documented JSON is not what the code produces")
    print("        produced : " + json.dumps(produced, sort_keys=True))
    print("        documented: " + json.dumps(documented, sort_keys=True))
    sys.exit(1)

print("  [PASS] the page's cpp block compiles and its documented output matches")
sys.exit(0)
PY
# the python step prints its own [PASS]/[FAIL] line; only tally the result
if [ "$?" -eq 0 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi

printf '\nDOCS EXAMPLE CHECK: %d passed, %d failed\n' "$pass" "$fail"
exit "$fail"
