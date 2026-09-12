# Measurement snapshots

Immutable session snapshots. Each file records the date, host, toolchain, git
revision and exact config in its header; regenerate with the script named in
that header rather than editing these in place.

| File | What it measures | Status |
|---|---|---|
| `measure_results_20260815.txt` | `bench_macro_vs_reflection.sh`: macro / v1 / refl2 at N=1…100, `-O0` and `-O2`, min-of-3 | ⚠️ **the `refl2`/`v2` column is invalid** — see `SCALING.md` §3 and the correction note in `EVALUATION.md` §2.2. The `macro` and `v1` columns are sound. |
| `runtime_measure_20260815.txt` | runtime throughput of the same three modes | macro/v1 sound; the refl2 rows come from the same mis-selected TU and should be re-derived. |
| `scaling_sweep_20260912.txt` | `bench_scaling.sh`: macro vs refl2 at N=100…1600, `-O0`, min-of-1 | current; refl2 struct is a plain annotated type (the real reflection path) |
| `scaling_sweep_o2_20260912.txt` | same at `-O2`, N=100…800 | current |

**Context for the ⚠️ rows.** `bench_macro_vs_reflection.cpp` selected the
struct definition for the refl2 mode with the macro branch, so every
"reflection" struct also had `NLOHMANN_DEFINE_TYPE_INTRUSIVE`. Its friend
`to_json` wins the codec's ADL branch (`priority_tag<4>`) over the reflection
branch (`priority_tag<1>`), so the reflection codec was never instantiated and
the refl2 numbers are the macro path. Fixed 2026-09-12. Analysis and corrected
measurements: `../SCALING.md`.
