# Scaling benchmark — does the reflection path blow up?

> **Status.** Generated 2026-09-12 on g++-16 16.2.0 (Ubuntu) / libstdc++ 16.
> Raw data: `data/scaling_sweep_20260912.txt` (`-O0`) and
> `data/scaling_sweep_o2_20260912.txt` (`-O2`). Regenerate with
> `tests/static-reflection/bench_scaling.sh`.
>
> **Why this exists.** `EVALUATION.md` §2.2 compares macro vs refl2 at
> N ≤ 100. The upstream adoption question is a different one: *does the cost
> stay linear as the type count grows, or does template instantiation blow
> up?* This sweep answers that, and along the way it found that the §2.2
> `refl2` column was not measuring what it claimed (§3 below).

## 1. Headline

**The reflection path scales essentially linearly — it does not blow up — but
its constant factor is much larger than the macro path's, and the previously
published comparison said otherwise because it mis-measured refl2.**

| | `refl2` (reflection) | `macro` (NLOHMANN_DEFINE_TYPE_*) |
|---|---|---|
| wall-time power-law slope (N=100…1600) | **0.944** | 0.692 |
| executable-size slope | **0.926** | 0.720 |
| symbol-count slope | **0.919** | 0.654 |
| ratio vs macro at N=1600 (`-O0`) | **5.5× wall · 5.8× size · 7.1× symbols** | 1× |

Slope 1.0 = linear. refl2 sits just under it; the macro path is *sub*linear
because it has a large fixed cost and a small marginal one, which is also why
the ratio keeps growing with N before stabilizing (2.7× at N=100 → 5.5× at
N=1600).

## 2. Measured data (`-O0`, min of 1 run)

| N | macro wall (s) | refl2 wall (s) | ratio | macro exe (KB) | refl2 exe (KB) | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 3.58 | 9.82 | 2.74× | 652 | 2115 | 3.24× |
| 200 | 4.58 | 17.56 | 3.83× | 926 | 3794 | 4.10× |
| 400 | 6.66 | 32.96 | 4.95× | 1473 | 7164 | 4.86× |
| 800 | 11.61 | 64.51 | 5.56× | 2568 | 13904 | 5.41× |
| 1600 | 24.76 | 134.93 | 5.45× | 4754 | 27400 | 5.76× |

Marginal wall cost per type is nearly flat for refl2 (0.0774 → 0.0880 s/type
across the range), which is the direct evidence for linearity. Peak RSS grows
1.7× (macro 1.69 GB → refl2 2.88 GB at N=1600).

At `-O2` (N=100…800) the wall-time ratio is *smaller* (2.7× → 3.3×) but the
size ratio is unchanged (~4.3× → 6.0×): the optimizer removes some of the
reflection scaffolding's cost in time, not in emitted code. Data:
`data/scaling_sweep_o2_20260912.txt`.

### What this means practically

At N=1600 the reflection path costs **135 s of compile time and a 27 MB
object** for a translation unit that does nothing but serialize 1600 trivial
five-field structs, against 25 s / 4.8 MB for the macro path. That is a real
adoption cost and should be stated as such — it is acceptable for a
few-hundred-type codebase and painful for a very large one. It is **not** a
complexity blowup.

## 3. The `refl2` column in the earlier comparison was invalid

`docs/static-reflection/data/measure_results_20260815.txt` reports, at N=20
`-O2`:
```
macro  N=20   O=2  exe=131088  nm=206
refl2  N=20   O=2  exe=131088  nm=206      <-- identical
```

That identity is real but meaningless: **the benchmark's `refl2` mode was
compiling the macro path.** `bench_macro_vs_reflection.cpp` chose its struct
definition with

```cpp
#ifdef BENCH_REFLECTION
    ... plain struct ...          // v1
#else
    ... NLOHMANN_DEFINE_TYPE_INTRUSIVE ...   // macro
#endif
```

and `BENCH_ADL_REFLECTION` (refl2) does not define `BENCH_REFLECTION`, so refl2
fell into the `#else` branch and every "reflection" struct also carried the
intrusive macro. That macro's friend `to_json` makes
`refl2::detail::to_adl_branch_eligible_v` **true**, and the codec's
`priority_tag<4>` ADL branch outranks its `priority_tag<1>` reflection branch —
so the reflection codec was never instantiated. Verified directly:

```
WithMacro: adl_branch_eligible=1  reflectable=1
Plain    : adl_branch_eligible=0  reflectable=1
```

Removing only the macro from the refl2 struct, at the identical configuration:

```
macro:  exe=131168  text=68379  nm=207     (reproduces the committed 131088/206)
refl2:  exe=308336  text=99509  nm=694     (committed said 131088/206)
```

So the published refl2 numbers were the macro path plus a thin codec wrapper.
**Any conclusion of the form "reflection costs about the same as the macros"
was drawn from a measurement that never exercised reflection.** Fixed in
`bench_macro_vs_reflection.cpp` (the refl2 branch now emits a plain annotated
struct) and in its driver, which now also passes the required
`-DJSON_USE_REFLECTION` (without it the TU does not compile at all — that
breakage had gone unnoticed because the last committed run predates the opt-in
gate).

### 3.1 The corrected N=1…100 table

Re-running the same sweep with the same driver at the same N and `-O`
(`data/measure_results_20260912_corrected.txt`) gives the numbers that replace
the invalid column. Executable bytes / `size text`, `-O2`:

| N | macro | v1 | refl2 | refl2 vs macro |
|---:|---:|---:|---:|---:|
| 1 | 113,464 | 119,888 | 121,536 | **+7%** |
| 20 | 131,168 | 174,424 | 308,336 | **+135%** |
| 28 | 145,200 | 198,312 | 384,656 | +165% |
| 50 | 182,864 | 269,488 | 623,280 | +241% |
| 100 | 266,888 | 434,144 | 1,155,184 | **+333% (4.3×)** |

and wall time (`-O2`, min of 3): 3.56 / 3.81 / 3.77 s at N=1 rising to
6.09 / 8.14 / **14.44** s at N=100 — refl2 is the slowest of the three at
every point, which is the opposite of what the invalid column implied.

The shape is worth stating plainly, because it differs from a fixed overhead:
the reflection path is **cheap for one type (+7% size, +6% time) and its cost
accumulates per type**. So "reflection costs about the same as the macro" is
true only for a handful of types, and badly wrong for hundreds — which is the
range a real codebase lives in.

## 4. Caveats

- **Single run per configuration** (`BENCH_RUNS=1`) to keep the sweep bounded;
  wall times are therefore noisier than the `-O0` snapshot's `min` of 3 runs in
  `measure_results_20260815.txt`. The trends are far larger than the noise, but
  treat absolute seconds as indicative.
- The generated struct shape matches `bench_macro_vs_reflection.cpp` exactly
  (five fields, including `std::string` and `std::vector<std::string>`), so the
  two harnesses are comparable.
- N counts **distinct types**, all instantiated for both `to_json` and
  `from_json`. A single type used many times is a different (cheaper)
  workload.
- Only GCC 16 was measured; P2996 implementations elsewhere may scale
  differently.
- The scaling harness deliberately uses the **annotated** struct for refl2
  (the shipped user-facing path), not a direct `refl2::codec<false>` call on a
  plain struct.
