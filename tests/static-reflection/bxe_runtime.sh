#!/usr/bin/env bash
# runtime protocol for bench_extended.cpp (EVALUATION.md §2.2 extended paths):
# RUNS binary runs per mode, min/median per direction. Build first:
#   g++-16 -std=c++26 -freflection -O2 -Iinclude \
#     -o build/scratch/bxe_macro tests/static-reflection/bench_extended.cpp
#   g++-16 -std=c++26 -freflection -O2 -Iinclude -DBENCH_ADL_REFLECTION \
#     -o build/scratch/bxe_refl2 tests/static-reflection/bench_extended.cpp
set -u
cd /home/joker/apps/json
RUNS="${RUNS:-5}"
declare -A LABEL=( [bxe_macro]="macro" [bxe_refl2]="refl2" )
DIRS=("inherited serialize" "inherited deserialize" "inherited round-trip"
      "optional serialize" "optional deserialize" "optional round-trip"
      "bitfield serialize" "bitfield deserialize" "bitfield round-trip")
for b in bxe_macro bxe_refl2; do
    declare -A samples
    samples=()
    for r in $(seq 1 "$RUNS"); do
        while read -r kind dir val unit; do
            samples["$kind $dir"]+="${val} "
        done < <(./build/scratch/$b 12345 | grep "us/op")
    done
    echo "== ${LABEL[$b]} =="
    for d in "${DIRS[@]}"; do
        vals="${samples[$d]:-}"
        [ -z "$vals" ] && { echo "  $d — (n/a)"; continue; }
        min=$(printf '%s\n' $vals | sort -n | head -1)
        med=$(printf '%s\n' $vals | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')
        printf "  %-22s min=%.3f median=%.3f us/op\n" "$d" "$min" "$med"
    done
done
