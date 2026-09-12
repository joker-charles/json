#!/usr/bin/env bash
# bench_macro_vs_reflection.sh — three-mode measurement driver for the
# EVALUATION.md §2.2 tables (reflection vs macros).
#
# Compiles tests/static-reflection/bench_macro_vs_reflection.cpp in the three
# modes (macro / refl v1 / refl2 v2) across a sweep of N (instantiated types)
# and optimization levels, and reports, per configuration, the minimum of
# BENCH_RUNS compile runs: wall time, peak RSS, executable bytes, `size`
# text, and `nm` symbol count — the exact metrics of §2.2.
#
# Usage:
#   bash tests/static-reflection/bench_macro_vs_reflection.sh          # full sweep
#   BENCH_N_SET="50" BENCH_RUNS=1 bash ...bench_macro_vs_reflection.sh  # quick
#   BENCH_MARKDOWN=1 bash ...bench_macro_vs_reflection.sh               # + §2.2 tables
#
# Environment overrides:
#   BENCH_N_SET   whitespace list of BENCH_N values  (default: "1 20 28 50 100")
#   BENCH_OPT_SET whitespace list of -O levels       (default: "0 2")
#   BENCH_RUNS    compile runs per config, min kept  (default: 3)
#   BENCH_MODES   modes to run                       (default: "macro refl refl2")
#   CXX           compiler binary                    (default: g++-16)
#   OUT_DIR       artifact directory (gitignored)    (default: build/scratch)
#   RESULTS_FILE  raw-runs output file               (default: $OUT_DIR/measure_results.txt)
#
# Requires GNU /usr/bin/time, size and nm. The full default sweep is ~90
# compiles (≈10 min on the branch machine); reduce BENCH_N_SET/BENCH_RUNS
# for a smoke run. Raw runs go to $RESULTS_FILE (regenerable working
# artifact); the authoritative session snapshot is committed under
# docs/static-reflection/data/ (see EVALUATION.md §5).
set -euo pipefail

BENCH_N_SET="${BENCH_N_SET:-1 20 28 50 100}"
BENCH_OPT_SET="${BENCH_OPT_SET:-0 2}"
BENCH_RUNS="${BENCH_RUNS:-3}"
BENCH_MODES="${BENCH_MODES:-macro refl refl2}"
CXX="${CXX:-g++-16}"
OUT_DIR="${OUT_DIR:-build/scratch}"
RESULTS_FILE="${RESULTS_FILE:-${OUT_DIR}/measure_results.txt}"
BENCH_MARKDOWN="${BENCH_MARKDOWN:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${SCRIPT_DIR}/bench_macro_vs_reflection.cpp"

# refl2 needs JSON_USE_REFLECTION: reflection_to_json.hpp is behind the opt-in
# gate, and the gate is read at include time.
declare -A MODE_FLAGS=( [macro]="" [refl]="-DBENCH_REFLECTION" [refl2]="-DBENCH_ADL_REFLECTION -DJSON_USE_REFLECTION" )
declare -A MODE_LABEL=( [macro]="macro" [refl]="refl v1" [refl2]="refl2 v2" )

# --- prerequisites ----------------------------------------------------------
command -v "${CXX}" >/dev/null 2>&1 || { echo "error: compiler '${CXX}' not found (try CXX=g++-16)" >&2; exit 1; }
[ -x /usr/bin/time ]            || { echo "error: GNU /usr/bin/time required (g++ measures wall/RSS with it)" >&2; exit 1; }
command -v size >/dev/null 2>&1 || { echo "error: 'size' not found" >&2; exit 1; }
command -v nm   >/dev/null 2>&1 || { echo "error: 'nm' not found" >&2; exit 1; }
[ -f "${SRC}" ]                 || { echo "error: benchmark TU not found: ${SRC}" >&2; exit 1; }

mkdir -p "${OUT_DIR}"

# per-config best values, keyed "n:o:mode"
declare -A WALL RSS EXE TEXT NM

# --- header (reproducibility context) ---------------------------------------
{
    echo "# refl2 three-mode bench sweep"
    echo "# date:      $(date -Is)"
    echo "# host:      $(hostname 2>/dev/null || echo n/a)"
    echo "# toolchain: $("${CXX}" --version 2>/dev/null | head -1 || echo n/a)"
    echo "# git:       $(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo n/a)"
    echo "# config:    N='${BENCH_N_SET}' O='${BENCH_OPT_SET}' runs=${BENCH_RUNS} modes='${BENCH_MODES}'"
    echo "# src:       ${SRC}"
    echo "# units:     wall=s, rss=KB, exe/text=bytes, nm=symbols"
    echo
} > "${RESULTS_FILE}"

for n in ${BENCH_N_SET}; do
    for o in ${BENCH_OPT_SET}; do
        for mode in ${BENCH_MODES}; do
            flags=(-std=c++26 -freflection -O"${o}" -DBENCH_N="${n}" -I"${REPO_ROOT}/include")
            # MODE_FLAGS entries may carry several flags; split on whitespace so
            # each becomes its own argv entry. Appending the raw string would
            # pass "-DA -DB" as ONE argument (a malformed -D) and the build
            # fails with no obvious cause.
            if [ -n "${MODE_FLAGS[$mode]}" ]; then
                read -r -a mode_flags <<< "${MODE_FLAGS[$mode]}"
                flags+=("${mode_flags[@]}")
            fi
            exe="${OUT_DIR}/bin_${mode}_n${n}_o${o}"
            best_wall=9999.0; best_rss=999999999; rc_ok=0
            tmp_err="${OUT_DIR}/.time_err_$$"
            for run in $(seq 1 "${BENCH_RUNS}"); do
                set +e
                /usr/bin/time -f '%e %M' "${CXX}" "${flags[@]}" \
                    -o "${exe}" "${SRC}" 2>"${tmp_err}" >/dev/null
                rc=$?
                set -e
                if [ "${rc}" -ne 0 ]; then
                    echo "BUILD FAIL n=${n} o=${o} mode=${mode} run=${run} — ${CXX} ${flags[*]}" >> "${RESULTS_FILE}"
                    rc_ok=1
                    break
                fi
                line="$(tail -1 "${tmp_err}")"
                wall="${line%% *}"; rss="${line##* }"
                if awk -v w="${wall}" -v bw="${best_wall}" 'BEGIN { exit (w >= bw) }'; then
                    best_wall="${wall}"
                fi
                if awk -v r="${rss}" -v br="${best_rss}" 'BEGIN { exit (r >= br) }'; then
                    best_rss="${rss}"
                fi
            done
            rm -f "${tmp_err}"
            if [ "${rc_ok}" -ne 0 ]; then
                continue
            fi
            WALL["${n}:${o}:${mode}"]="${best_wall}"
            RSS["${n}:${o}:${mode}"]="${best_rss}"
            EXE["${n}:${o}:${mode}"]="$(stat -c %s "${exe}")"
            TEXT["${n}:${o}:${mode}"]="$(size "${exe}" | tail -1 | awk '{print $1}')"
            NM["${n}:${o}:${mode}"]="$(nm "${exe}" | wc -l)"
            printf '%-6s N=%-4s O=%-2s wall_min=%-8s rss_min=%-9s exe=%-10s text=%-10s nm=%s\n' \
                "${mode}" "${n}" "${o}" "${best_wall}" "${best_rss}" \
                "${EXE[$n:$o:$mode]}" "${TEXT[$n:$o:$mode]}" "${NM[$n:$o:$mode]}" \
                >> "${RESULTS_FILE}"
            echo "done: ${mode} N=${n} O=${o} wall=${best_wall} rss=${best_rss} exe=${EXE[$n:$o:$mode]}"
        done
    done
done

# --- behavior parity: the three modes must print the same counter -----------
parity_ok=1
for n in ${BENCH_N_SET}; do
    for o in ${BENCH_OPT_SET}; do
        totals=""
        for mode in ${BENCH_MODES}; do
            exe="${OUT_DIR}/bin_${mode}_n${n}_o${o}"
            [ -x "${exe}" ] || continue
            totals="${totals} $("${exe}")"
        done
        # keep only the total=N part of each line
        nums="$(printf '%s\n' ${totals} | sed -n 's/.*total=\([0-9]*\).*/\1/p' | sort -u | wc -l)"
        if [ "${nums}" -gt 1 ]; then
            echo "PARITY WARNING: N=${n} O=${o} modes disagree:${totals}" >> "${RESULTS_FILE}"
            parity_ok=0
        fi
    done
done
if [ "${parity_ok}" -eq 1 ]; then
    echo "# behavior parity: all modes print identical totals" >> "${RESULTS_FILE}"
fi

# --- informational: macro vs refl2 at -O2 (headline of §2.2) ----------------
# The claim is SIZE identity with near-identical codegen: exe bytes exactly
# equal at every N, `size text` within 32 B. True byte-identity is not
# expected — the linker build-id note hashes the TU content (macro and refl2
# sources necessarily differ) and the read-only layout can shift by a 32-B
# alignment gap. The check below strips the build-id note before comparing.
for n in ${BENCH_N_SET}; do
    m="${OUT_DIR}/bin_macro_n${n}_o2"; r2="${OUT_DIR}/bin_refl2_n${n}_o2"
    [ -x "${m}" ] && [ -x "${r2}" ] || continue
    sz_m="$(stat -c %s "${m}")"; sz_r="$(stat -c %s "${r2}")"
    if [ "${sz_m}" = "${sz_r}" ]; then sz="size-identical (${sz_m} B)"; else sz="size differ (${sz_m} vs ${sz_r})"; fi
    objcopy --remove-section .note.gnu.build-id "${m}" "${OUT_DIR}/.cmp_m_$$" 2>/dev/null || true
    objcopy --remove-section .note.gnu.build-id "${r2}" "${OUT_DIR}/.cmp_r2_$$" 2>/dev/null || true
    if [ -f "${OUT_DIR}/.cmp_m_$$" ] && [ -f "${OUT_DIR}/.cmp_r2_$$" ] && cmp -s "${OUT_DIR}/.cmp_m_$$" "${OUT_DIR}/.cmp_r2_$$"; then
        code="byte-identical modulo .note.gnu.build-id"
    else
        t_m="$(size "${m}" | tail -1 | awk '{print $1}')"; t_r="$(size "${r2}" | tail -1 | awk '{print $1}')"
        code="text ${t_m} vs ${t_r} B ($(( t_r - t_m )) B delta)"
    fi
    rm -f "${OUT_DIR}/.cmp_m_$$" "${OUT_DIR}/.cmp_r2_$$"
    echo "# -O2 N=${n}: macro vs refl2 — ${sz}; ${code}" >> "${RESULTS_FILE}"
done

echo "results: ${RESULTS_FILE}"
echo "SWEEP DONE" >> "${RESULTS_FILE}"

# --- markdown summary (paste-ready §2.2 tables) ------------------------------
if [ "${BENCH_MARKDOWN}" = "1" ]; then
    printf '\n### Q2 compile time (min of %s):\n\n' "${BENCH_RUNS}"
    printf '| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |\n'
    printf '|---|---|---|---|---|---|---|\n'
    for n in ${BENCH_N_SET}; do
        printf '| %s |' "${n}"
        for o in 0 2; do
            for mode in macro refl refl2; do
                printf ' %s s |' "${WALL[${n}:${o}:${mode}]:-?}"
            done
        done
        printf '\n'
    done
    printf '\n### Q3 binary (executable bytes / size text):\n\n'
    printf '| N | macro -O0 | v1 -O0 | v2 -O0 | macro -O2 | v1 -O2 | v2 -O2 |\n'
    printf '|---|---|---|---|---|---|---|\n'
    for n in ${BENCH_N_SET}; do
        printf '| %s |' "${n}"
        for o in 0 2; do
            for mode in macro refl refl2; do
                printf ' %s / %s |' "${EXE[${n}:${o}:${mode}]:-?}" "${TEXT[${n}:${o}:${mode}]:-?}"
            done
        done
        printf '\n'
    done
    printf '\n'
fi
