#!/usr/bin/env bash
# namespace_compile_fail.sh — negative harness for the reflection-namespace
# opt-in front-end.
#
# Each tests/static-reflection/namespace_compile_fail/*.cpp MUST fail to
# compile, and its diagnostic MUST contain the substring on its `EXPECT-DIAG:`
# line. Wide/unconstrained cases declare JSON_REFLECTION_NAMESPACE_UNCONSTRAINED
# in-source; the runner always defines JSON_USE_REFLECTION.
#
# Run: GXX=g++-16 bash tests/static-reflection/namespace_compile_fail.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
INCS=(-std=c++26 -freflection -fsyntax-only -DJSON_USE_REFLECTION -I"$REPO/include")

CASES=("$SCRIPT_DIR"/namespace_compile_fail/*.cpp)
total=0
pass=0
fail=0

for src in "${CASES[@]}"; do
    [ -e "$src" ] || continue
    name="$(basename "$src")"
    expect="$(grep -m1 -oE 'EXPECT-DIAG:.*' "$src" | sed 's/EXPECT-DIAG:[[:space:]]*//')"
    if [ -z "$expect" ]; then
        printf '  [SKIP] %-38s (no EXPECT-DIAG line)\n' "$name"
        continue
    fi
    total=$((total + 1))
    out="$("$GXX" "${INCS[@]}" "$src" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        printf '  [FAIL] %-38s compiled successfully (expected: "%s")\n' "$name" "$expect"
        fail=$((fail + 1))
        continue
    fi
    if printf '%s' "$out" | grep -qF -- "$expect"; then
        printf '  [PASS] %-38s fails with: "%s"\n' "$name" "$expect"
        pass=$((pass + 1))
    else
        printf '  [FAIL] %-38s missing diagnostic: "%s"\n' "$name" "$expect"
        printf '%s\n' "$out" | grep -m1 -E 'error:' | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done

# The feature macro without P2996 must fail with the documented #error through
# the namespace header too, with exactly one diagnostic.
gate_src="$(mktemp)"
gate_err="$(mktemp)"
trap 'rm -f "$gate_src" "$gate_err"' EXIT
printf '#define JSON_USE_REFLECTION\n#include <nlohmann/reflection_namespace.hpp>\nint main() {}\n' > "$gate_src"
"$GXX" -std=c++26 -x c++ -fsyntax-only -I"$REPO/include" "$gate_src" 2>"$gate_err"
gate_rc=$?
gate_errors="$(grep -c 'error:' "$gate_err" || true)"
total=$((total + 1))
if [ "$gate_rc" -eq 0 ]; then
    printf '  [FAIL] %-38s compiled successfully (expected the P2996 #error)\n' "gate_ns_no_p2996.cpp"
    fail=$((fail + 1))
elif grep -qF -- 'requires C++26 static reflection (P2996)' "$gate_err" && [ "$gate_errors" -eq 1 ]; then
    printf '  [PASS] %-38s fails with a single actionable #error\n' "gate_ns_no_p2996.cpp"
    pass=$((pass + 1))
else
    printf '  [FAIL] %-38s missing the documented #error or extra diagnostics (%s)\n' "gate_ns_no_p2996.cpp" "$gate_errors"
    fail=$((fail + 1))
fi

printf '\nNAMESPACE COMPILE-FAIL HARNESS: %d/%d passed, %d failed\n' "$pass" "$total" "$fail"
exit "$fail"
