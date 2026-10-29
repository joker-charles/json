#!/usr/bin/env bash
# compile_fail.sh — negative compile harness for the refl2 catch-all guards.
#
# Each tests/static-reflection/compile_fail/*.cpp MUST fail to compile, and its
# diagnostic MUST contain the substring declared on its `EXPECT-DIAG:` line.
# This is what proves the static_assert rejection guards actually FIRE (a
# Boolean trait probe only proves the *detection* exists, not the *rejection*).
#
# Run:  GXX=g++-16 ./tests/static-reflection/compile_fail.sh
# (from the repository root; g++-16 -std=c++26 -freflection required)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
INCS=(-std=c++26 -freflection -fsyntax-only -I"$REPO/include")

CASES=("$SCRIPT_DIR"/compile_fail/*.cpp)
total=0
pass=0
fail=0

for src in "${CASES[@]}"; do
    [ -e "$src" ] || continue
    name="$(basename "$src")"
    expect="$(grep -m1 -oE 'EXPECT-DIAG:.*' "$src" | sed 's/EXPECT-DIAG:[[:space:]]*//')"
    if [ -z "$expect" ]; then
        printf '  [SKIP] %-26s (no EXPECT-DIAG line)\n' "$name"
        continue
    fi
    total=$((total + 1))
    out="$("$GXX" "${INCS[@]}" "$src" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        printf '  [FAIL] %-26s compiled successfully (expected compile error: "%s")\n' "$name" "$expect"
        fail=$((fail + 1))
        continue
    fi
    if printf '%s' "$out" | grep -qF -- "$expect"; then
        printf '  [PASS] %-26s fails with: "%s"\n' "$name" "$expect"
        pass=$((pass + 1))
    else
        printf '  [FAIL] %-26s fails to compile but missing diagnostic: "%s"\n' "$name" "$expect"
        printf '%s\n' "$out" | grep -m1 -E 'error:' | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done

printf '\nCOMPILE-FAIL HARNESS: %d/%d passed, %d failed\n' "$pass" "$total" "$fail"
exit "$fail"
