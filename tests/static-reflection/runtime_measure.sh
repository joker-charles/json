#!/usr/bin/env bash
# runtime protocol: N binary runs per variant; per direction report
# min/median of the internal medians. Measurement driver for
# bench_runtime.cpp (EVALUATION.md §2.2 runtime; builds go to build/scratch).
#
# Build the four binaries first (see bench_runtime.cpp header):
#   g++-16 -std=c++26 -freflection -O2 -Iinclude -o build/scratch/brt_macro \
#     tests/static-reflection/bench_runtime.cpp
#   g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_REFLECTION \
#     -o build/scratch/brt_BENCH_REFLECTION tests/static-reflection/bench_runtime.cpp
#   g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
#     -o build/scratch/brt_BENCH_ADL_REFLECTION tests/static-reflection/bench_runtime.cpp
#   # "old codec" baseline (pre-optimization codec, commit 62290f3b):
#   g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
#     -o build/scratch/brt_old_v2 tests/static-reflection/bench_runtime_old.cpp
# RUNS=7 (default) / RUNS=11 for tighter confidence.
set -u
cd /home/joker/apps/json
RUNS="${RUNS:-7}"

declare -A LABEL=( [brt_macro]="macro" [brt_BENCH_REFLECTION]="refl v1" \
                   [brt_old_v2]="refl2 v2 (old codec)" [brt_BENCH_ADL_REFLECTION]="refl2 v2 (optimized)" )
DIRS=("flat serialize" "flat deserialize" "nested serialize" "nested deserialize")

for b in brt_macro brt_BENCH_REFLECTION brt_old_v2 brt_BENCH_ADL_REFLECTION; do
    declare -A samples
    samples=() # reset: `declare -A` does NOT clear an existing global array
    for r in $(seq 1 "$RUNS"); do
        while read -r kind dir val unit; do
            samples["$kind $dir"]+="${val} "
        done < <(./build/scratch/$b 12345 | grep "us/op")
    done
    echo "== ${LABEL[$b]} =="
    for d in "${DIRS[@]}"; do
        vals="${samples[$d]:-}"
        if [ -z "$vals" ]; then
            echo "  $d — (n/a)"
            continue
        fi
        min=$(printf '%s\n' $vals | sort -n | head -1)
        med=$(printf '%s\n' $vals | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')
        printf "  %s  min=%.3f median=%.3f us/op\n" "$d" "$min" "$med"
    done
done
