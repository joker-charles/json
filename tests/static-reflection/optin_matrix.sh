#!/usr/bin/env bash
# optin_matrix.sh — one command for the opt-in comparison evidence.
#
# Runs the annotation baseline, the reflection-namespace runtime fixtures in
# both include layouts, the precedence matrix, the ODR/visibility TUs, and both
# negative-compile harnesses.
#
# Usage:
#   GXX=g++-16 bash tests/static-reflection/optin_matrix.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GXX="${GXX:-g++-16}"
OUT="${OUT:-$REPO/build/optin-matrix}"
mkdir -p "$OUT"

CXX=(-std=c++26 -freflection -O1 -g -fsanitize=address,undefined)
INCS=(-I"$REPO/include")
INCS_SINGLE=(-I"$REPO/single_include" -I"$REPO/include")

pass=0
fail=0

run() { # name, command...
    local name="$1"
    shift
    if "$@" >"$OUT/$name.log" 2>&1; then
        printf '  [PASS] %s\n' "$name"
        pass=$((pass + 1))
    else
        printf '  [FAIL] %s (see %s/%s.log)\n' "$name" "$OUT" "$name"
        tail -n 8 "$OUT/$name.log" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
}

build_run() { # name, source, includes...
    local name="$1"
    local src="$2"
    shift 2
    if ! "$GXX" "${CXX[@]}" "$@" -o "$OUT/$name" "$src" >"$OUT/$name.build.log" 2>&1; then
        printf '  [FAIL] %s (build; see %s/%s.build.log)\n' "$name" "$OUT" "$name"
        tail -n 8 "$OUT/$name.build.log" | sed 's/^/        /'
        fail=$((fail + 1))
        return
    fi
    run "$name" "$OUT/$name"
}

printf '== annotation baseline ==\n'
build_run reflection_flat_struct "$SCRIPT_DIR/reflection_flat_struct.cpp" "${INCS[@]}"
build_run reflection_flat_struct_single "$SCRIPT_DIR/reflection_flat_struct.cpp" "${INCS_SINGLE[@]}"

printf '\n== reflection-namespace runtime fixtures ==\n'
build_run reflection_namespace "$SCRIPT_DIR/reflection_namespace.cpp" "${INCS[@]}"
build_run reflection_namespace_single "$SCRIPT_DIR/reflection_namespace.cpp" "${INCS_SINGLE[@]}"
build_run optin_conflicts "$SCRIPT_DIR/optin_conflicts.cpp" "${INCS[@]}"
build_run optin_namespace_wide "$SCRIPT_DIR/optin_namespace_wide.cpp" "${INCS[@]}"

printf '\n== contract evidence (Q2 boundaries, Q3 semantics, Q4 migration) ==\n'
build_run boundary_matrix "$SCRIPT_DIR/boundary_matrix.cpp" "${INCS[@]}"
build_run semantics_matrix "$SCRIPT_DIR/semantics_matrix.cpp" "${INCS[@]}"
build_run migration_matrix "$SCRIPT_DIR/migration_matrix.cpp" "${INCS[@]}"

printf '\n== ODR / visibility TUs (each must compile independently) ==\n'
run odr_a "$GXX" -std=c++26 -freflection -fsyntax-only "${INCS[@]}" "$SCRIPT_DIR/optin_odr/a.cpp"
run odr_b "$GXX" -std=c++26 -freflection -fsyntax-only "${INCS[@]}" "$SCRIPT_DIR/optin_odr/b.cpp"
printf '\n== negative-compile harnesses ==\n'
run compile_fail_annotation bash "$SCRIPT_DIR/compile_fail.sh"
run compile_fail_namespace bash "$SCRIPT_DIR/namespace_compile_fail.sh"

printf '\nOPT-IN MATRIX: %d passed, %d failed\n' "$pass" "$fail"
exit "$fail"
