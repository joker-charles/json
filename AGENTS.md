# AGENTS.md — nlohmann/json · feature/static-reflection branch

This file captures the working conventions for the `feature/static-reflection`
branch. It is the authoritative operational guide for any agent/human entering
this branch: what the work is, which toolchain to trust, how to build/reproduce
the verified fixtures, and — crucially — the safety boundaries you must not
cross when touching the main library.

> **Scope.** The repository is nlohmann/json (3.12.0). This document describes
> the **branch-specific experimental work** on top of `develop` (cdf52ae9). It
> does not restate upstream contribution rules (see `.github/CONTRIBUTING.md`).

## 1. What this branch is

Two coupled goals:

1. **Static reflection (P2996)** — explore rewriting the tagged-union core with
   C++26 reflection, as a self-contained study (not a replacement of the C++11
   library).
2. **C++20 concepts modernization** — replace hand-written SFINAE detection with
   concepts for the pure-category overloads in the serialization layer, under a
   `JSON_HAS_CPP_20` dual path.

Status: M0–M3 (reflection) complete & verified; M4 verdict: reflection does not
replace type_traits, classification in `docs/static-reflection/M4_ASSESSMENT.md`;
to_json concepts dual-path landed and committed. **M5 shipped**: the refl2
codec moved into `include/nlohmann/reflection_to_json.hpp` (new experimental
header) with a `json_name`/`json_ignore`/`json_default` annotation layer, and
is wired into `detail::{to,from}_json` as reflection-gated catch-alls
(`__cpp_impl_reflection && __cpp_lib_reflection`, i.e. only g++-16
`-std=c++26 -freflection`) — arbitrary reflectable structs serialize
bidirectionally with zero user code, replacing the NLOHMANN_DEFINE_TYPE_*
macro family (Scenario B of EVALUATION.md); **M6**: enumerator-level
`json_name` annotations replace NLOHMANN_JSON_SERIALIZE_ENUM (compile-time
enumerator tables; unannotated enums keep the integer path, zero drift).
See
`docs/static-reflection/M5_REFLECTION_TO_JSON.md` and the doc map (§6).

## 2. Toolchain — trust, don't re-derive

- Use **`g++-16`** (GCC 16.1.0). The system default `g++` is 15 and **has no
  `<meta>`** (nor other C++26 headers) — reflection code will fail with
  `fatal error: meta: No such file or directory` if compiled with the wrong
  compiler.
- Reflection build flags: `-std=c++26 -freflection`.
- Smoke test (one line, cheap confidence — not a full re-verification):
  ```sh
  printf 'int main(){}' | g++-16 -std=c++26 -freflection -x c++ - -o /tmp/t && echo "g++-16 OK"
  ```
- CMake presets (`CMakeUserPresets.json`, gitignored): `cxx26-reflect` /
  `cxx26-reflect-tests` pin `g++-16` + `CMAKE_CXX_STANDARD=26` (+
  `JSON_TestStandards=26` for the suite). **Do NOT put `-freflection` into
  `CMAKE_CXX_FLAGS`** — it leaks into the nested-project cmake tests
  (`tests/cmake_add_subdirectory` etc.), whose `TryCompile` probes compile with
  the *default* standard and die with `'-freflection' only supported with
  '-std=c++26'` (this exact failure broke 13 ctest cases until the preset was
  fixed). Reflection is consumer-side: add `-freflection` only to the TUs that
  actually use P2996 (the reflection fixtures do this via their own build
  lines).
- Two include layouts: split headers under `-Iinclude`, or the amalgamated
  single header under `-Isingle_include`. Reflection/concepts work targets the
  split `-Iinclude` layout (the `#ifdef JSON_HAS_CPP_20` branches only compile
  there).

## 3. Facts verified on this toolchain (do not re-verify)

Same toolchain ⇒ trust these; re-deriving them is wasted work.

- **Reflection/P2996** (verified by `tests/static-reflection/probe_*.cpp`):
  - `std::meta::` must be fully qualified; bare `meta::` does not compile.
  - `basic_json::json_value` / `basic_json::data` are **private nested types**.
    A direct scope-splice `[: ^^ json::json_value :]` FAILS ("is private within
    this context"). **BUT** — verified by `probe_real_json_value.cpp` — they are
    fully reachable indirectly: `unchecked()` reflection enumerates the private
    data members (`data` via `m_data`, then `json_value` via `data::m_value`),
    `type_of` yields the private nested type, and it can be spliced for full use
    (enumerate members, construct via `value_t`, read/write members, allocate/
    free pointer members). **Key GCC-16 trap**: obtaining the type works both
    from a `consteval` function and inside `template for`, but *enumerating its
    members* works ONLY from a `consteval` context — inside `template for` the
    indirectly-obtained type reports "not a complete class type". So the
    `json_value_mirror` route is a workaround, not a hard requirement; a
    consteval-helper design can reflect the real `json_value` directly.
  - `nonstatic_data_members_of` works on a union; `is_pointer_v<type_of(m)>`
    classifies storage category.
  - Reflection queries returning a `std::vector` are **transient**: subscript/
    size directly on the call; never bind to a local constexpr (GCC rejects with
    "refers to a result of 'operator new'").
  - `template for` exists only in range form; the range must be an inline
    `define_static_array(...)` expression or a namespace-scope/static constexpr.
  - `if constexpr` does NOT discard the false branch; dispatch by tag overload /
    partial specialization.
  - Every member/base query takes TWO arguments — `(info, access_context)` —
    with no default: `members_of`, `bases_of`, `nonstatic_data_members_of`,
    `subobjects_of`, `static_data_members_of` (GCC rejects the one-arg form).
  - `subobjects_of(^^T, ctx)` is the **unified list** for inheritance: direct
    base subobjects first, then direct non-static data members, access-
    filtered (`unprivileged()` = public only; `unchecked()` = everything).
    `is_base(info)` distinguishes base entries from member entries. A derived
    type's members are NOT in `nonstatic_data_members_of` (base members
    silently dropped) — recurse `subobjects_of(type_of(base_info), ctx)`
    depth-first, base-before-member, for full inheritance; the member-access
    splice `v.[:m:]` works for members of base classes too.
  - **`bases_of` enumeration trap** (verified; real issue, cf. LLVM #172136):
    an info from `bases_of(...)[i]` is not enumerable directly —
    `nonstatic_data_members_of(b0, ctx)` throws `not a complete class type`
    even in a consteval function. Recover the type first
    (`using B = typename [: type_of(b0) :];`), or pre-check with
    `is_enumerable_type` (false for incomplete types). `is_complete_type`
    also available.
  - **Access-classification predicates need no context**: `is_public` /
    `is_protected` / `is_private` (and `is_virtual`) on base/member infos.
    `has_inaccessible_bases` / `has_inaccessible_nonstatic_data_members` /
    `has_inaccessible_subobjects` are the dedicated "private/protected
    present" detectors. Protected entities need a naming-class context
    (`access_context::via(^^Derived)`) — `unprivileged()` cannot see them.
  - **Virtual bases duplicate on flatten**: `subobjects_of` reports each
    virtual-base relationship, so a shared virtual base (diamond + virtual
    inheritance) flattens its members multiple times while C++ has ONE such
    subobject — deduplicate or compile-error; don't let it surface as a
    duplicate-key error.
  - **Bit-fields** (`is_bit_field`): serialize fine (const-ref copy), but
    `from_json` cannot bind them to a `T&` (no address — macro paths have the
    same limit); assign via `j.at(k).get<M>()`. Unnamed bit-fields are NOT
    subobjects and are skipped by `subobjects_of`. A class with a
    private/protected base is NOT an aggregate (C++17) — needs a ctor.
  - **Tag-dispatch pitfall**: a dispatcher forwarding to tagged overloads
    (`m(j, v, std::bool_constant<flag>{})`) fails to deduce the non-type
    template parameter `I` even though `bool_constant<false>` IS
    `std::false_type` — pass the explicit list: `m<B, T, I>(j, v, tag)`.
- **Concepts dual-path** (verified by `concepts_smoke.cpp` + layered probes):
  - `concepts::array_like/object_like/string_like` are **byte-identical** to the
    traits only when they use the *exact* probe targets: for `array_like` the
    iterator must come from **`begin-range` (`is_range`/`range_value_t`)**, NOT
    `T::iterator` — range views like `std::ranges::reverse_view<ref_view<json>>`
    have `begin()` but no `iterator` alias; the baseline trait accepts them while
    a `T::iterator` probe wrongly rejects them (a real drift that broke
    `test-iterators2_cpp20`, fixed in commit 4fe8a702). Use `is_constructible`
    (not `convertible_to`), and preserve the `vector<uint8_t>`-is-not-binary
    special case. A "semantic restatement" drifts (see `probe_draft_drift.cpp`:
    1 real drift on `vector<uint8_t>` binary).
  - `#if` cannot appear inside a `requires` clause ⇒ gate the range-view
    exclusion through a `bool` variable template (`not_range_view`).
  - A multi-condition `requires` chain must be wrapped in parentheses:
    `requires (A && B && ...)`.
  - **Partial-application concepts carry `BasicJsonType` LAST.** `string_like` /
    `object_like` / `array_like` are declared `template<T /*candidate*/, B
    /*BasicJsonType*/>`. GCC resolves a constrained placeholder
    `concepts::string_like<BasicJsonType> S` by binding `S` to the FIRST param
    and the explicit `<BasicJsonType>` to the SECOND — declaring them `<B, T>`
    puts the wrong type in `B` and `typename B::string_t` fails on a user type
    like a custom `alt_string` string_t (real regression: `test-alt-string_cpp26`
    failed; verified minimal repro + `concepts::X<T, B>` order fixed both the
    placeholder form and the explicit `requires(X<T, B> ...)` form). Never
    reorder these back to `<B, T>`. Do NOT leave this class of bug to be caught
    only by a non-default-string_t TU: `concepts_smoke`/dual tests use
    `std::string` where the wrong binding happens to agree, so they miss it.
- **M5 reflection catch-all** (verified by `probe_reflection_replace_macros.cpp`
  + `probe_adl_recursion.cpp`; design in `docs/static-reflection/M5_REFLECTION_TO_JSON.md`):
  - **Gate macros**: `-freflection` is what defines `__cpp_impl_reflection` /
    `__cpp_lib_reflection` — plain `-std=c++26` defines neither and `<meta>` is
    empty. The to_json/from_json catch-alls are gated on
    `defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)`.
  - **The whole user-type chain hooks in with ONE overload**: `json j = s` goes
    `is_compatible_type` (= `has_to_json`) → `adl_serializer<T,void>::to_json`
    → CPO `::nlohmann::to_json` → `to_json_fn::operator()` → unqualified
    `detail::to_json` (ordinary lookup = the overload set declared before
    `to_json_fn`). A constrained catch-all declared before `to_json_fn` /
    `from_json_fn` (the gated include sits at the top of those files) is found
    by ordinary lookup; user free `to_json` is found by ADL at instantiation;
    `adl_serializer<T,void>` specializations are called directly by the
    constructor/get and never reach `detail::to_json`.
  - **`&&` short-circuit does NOT stop template recursion**: `E1 && E2` where
    `E2` is `SomeTrait<T>::value` instantiates `SomeTrait<T>` before evaluation
    ("recursively required by substitution"). Circularity must be cut at the
    probe level, not by operand ordering.
  - **The catch-all makes the CPO path valid for plain structs**, so any probe
    through `adl_serializer`/the CPO recurses. Three cuts: (1) user-customization
    probes live in a private namespace (`refl2::detail::adl_probe`) where
    ordinary lookup sees nothing and ADL never sees `nlohmann::detail` for
    USER types — pure ADL detection of free functions; (2) library-internal
    types (`identity_tag<T>`, `initializer_list<json_ref<json>>`, ...) ARE in
    the associated namespace of `nlohmann::detail` via themselves or their
    template arguments, so the probe re-enters the catch-all — exclude them
    with `in_json_namespace(^^T)` (parent-chain walk comparing
    `identifier_of` to `"nlohmann"`, recursing into `template_arguments_of`,
    guarding `has_identifier` — the global namespace has none) placed FIRST in
    the requires AND as a `false_type` partial specialization of the probes
    (no probing at all); (3) the codec's own adl branch excludes
    `eligible && adl_serializer_is_primary` — `adl_serializer_is_primary`
    probes `&adl_serializer<T,void>::template to_json<B,T>` (addressable only
    for the primary's member template, not a user specialization with a
    non-template static to_json), so specialized types still take the adl
    branch (customization wins) while plain structs are member-reflected.
  - **from_json exclusion set must NOT use `is_getable`** (it probes
    `j.get<T>()`, which goes through the catch-all — the same cycle);
    containers are excluded structurally (`is_array_like`/`is_object_like`/
    `is_optional`/string probes mirroring each side's own overload probes).
  - **Annotation types must be structural AND extractable**: `std::string`/
    `std::string_view` members are rejected ("does not have structural type" —
    libstdc++ members are private); `const char*` members make
    `meta::extract` throw "reflect_constant failed"; string literals can never
    be template arguments. `json_name` uses a fixed `char value[64]` array
    (structural + extractable). Annotation reading is query-domain only:
    direct subscript of the transient `annotations_of` + `meta::remove_cvref`
    (annotation types are cv/ref-qualified) + `meta::is_same_type` +
    `extract<json_name>` — NO splices (a splice needs the entity as a
    constant expression, which a consteval function parameter is not) and NO
    `template for` (its range needs a constant too). Bind the WHOLE extract
    result, not its array subobject ("accessing `<anonymous>` outside its
    lifetime"). Member keys travel as value-copied `std::array<char,64>`
    (a string_view into the extract temporary is not a constant expression
    and dangles).
  - `adl_serializer<T, B>` with an explicit second argument is a historical
    refl2 convention; the library's real customization surface is
    `adl_serializer<T, void>` (json_serializer<T, void>) — the codec now calls
    `<T, void>` and probes/specializations must match.
  - **Enum string mapping (M6)**: enumerator-level `json_name` annotations
    require the attribute AFTER the identifier
    (`green [[=refl2::json_name{"GREEN"}]] = 2`); before the identifier is
    rejected by GCC 16. `constant_of` on an enumerator yields an `info`, NOT
    the value — take the value via an enumerator SPLICE in a variable
    template (`[: enumerators_of(^^E)[I] :]`). Any annotated enumerator
    switches the whole enum to string mapping (unannotated enumerators fall
    back to `identifier_of`); a fully unannotated enum keeps the integer
    path byte-for-byte.

## 4. Build & reproduce

Reference for the artifacts (all under `g++-16`):

```sh
# M1 enum reflection + probes
g++-16 -std=c++26 -freflection -O2 -Isingle_include -Iinclude -o /tmp/p repro_m1.cpp
# M2/M3 differential (vs the real library) + ASan leak check
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/d tests/static-reflection/m2_diff.cpp && /tmp/d
# M4 concepts dual-path smoke (compile for -std=c++11/20/26; outputs must match)
g++-16 -std=c++20 -O0 -Iinclude -o /tmp/s tests/static-reflection/concepts_smoke.cpp && /tmp/s
# M5 macro<->annotation differential + zero drift (reflection vs baseline)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/prm_r tests/static-reflection/probe_reflection_replace_macros.cpp && /tmp/prm_r
g++-16 -std=c++26 -O1 -Iinclude \
  -o /tmp/prm_b tests/static-reflection/probe_reflection_replace_macros.cpp && /tmp/prm_b
#   diff <(grep '^COMMON' <(./prm_b)) <(grep '^COMMON' <(./prm_r)) must be empty
# M6 enum string mapping (macro<->annotation + zero drift)
g++-16 -std=c++26 -freflection -O1 -g -fsanitize=address -Iinclude \
  -o /tmp/per_r tests/static-reflection/probe_enum_reflection.cpp && /tmp/per_r
g++-16 -std=c++26 -O1 -Iinclude \
  -o /tmp/per_b tests/static-reflection/probe_enum_reflection.cpp && /tmp/per_b
```

The full fixture list lives in `tests/static-reflection/`; each file documents
its own build line in its header comment. The main-library changes are also
covered by the repository's doctest unit suite — see §5 "Repository test suite".

## 5. Safety boundaries (non-negotiable)

- **Main library `include/nlohmann/detail/conversions/*.hpp` and the traits layer
  are C++11-contract.** Any change must compile cleanly across
  `-std=c++11/14/17/20/26`, or be gated behind `#ifdef JSON_HAS_CPP_20` with a
  full C++11 fallback. The dual-track pattern in those files is the template to
  follow.
- **`single_include/nlohmann/json.hpp` is an amalgamated build artifact**, a
  snapshot of the split headers. Do not edit it directly for feature work; if you
  touch a split header read into the single header, **re-amalgamate** so the
  single header stays in sync:
  ```sh
  make amalgamate   # amalganmate.py + `make pretty` (astyle 3.4.13 via tools/astyle/venv)
  ```
  astyle 3.4.13 (the CI-pinned version) and the venv are set up in this
  workspace (`tools/astyle/venv`, gitignored), so `make amalgamate` and `make
  pretty` are fully reproducible locally. If you only need the single header
  regenerated without the astyle pass, run:
  ```sh
  python3 tools/amalgamate/amalgamate.py -c tools/amalgamate/config_json.json -s .
  ```
  Note: `make pretty` formats **all** `include/`, `tests/`, and `docs/examples/`
  sources; review the diff and keep unrelated pre-existing files out of the
  commit (see da68ff71 for the cleanup of docs/examples/parser_callback_t.cpp).
- **Repository test suite.** The main-library changes are exercised by the
  repository's doctest suite. Configure and build a subset, e.g.:
  ```sh
  cmake -S . -B build-test -DCMAKE_CXX_COMPILER=g++-16 \
    -DJSON_BuildTests=ON -DJSON_MultipleHeaders=ON -DBUILD_TESTING=ON
  cmake --build build-test --target test-udt_cpp11 test-serialization_cpp11 \
    test-conversions_cpp17 test-concepts_dual_cpp20 -j2
  ./build-test/tests/test-concepts_dual_cpp20
  ```
  `tests/src/unit-concepts_dual.cpp` (mentions `JSON_HAS_CPP_20`) registers
  both `_cpp11` and `_cpp20` targets and proves the dual path is behavior-preserving.
- **Free-experiment region** (safe to change freely): `concepts.hpp`,
  `reflection_json.hpp`, `reflection_to_json.hpp`, everything under
  `tests/static-reflection/`, and the docs under `docs/static-reflection/`.
- **Every new overload / concept / trait rewrite must be validated** by the
  differential / zero-drift probes outlined in `M4_ASSESSMENT.md` §7. The burden
  is on the change to prove byte-identical behavior, not on reviewers to trust it.
- **Session scratch files live in `.tmp/` or `build/scratch/` (both
  gitignored).** `/tmp` does not survive across tool invocations in this
  environment — never rely on it for anything you need again (experiment
  TUs, negative compile cases, one-off probes, measurement artifacts).
  `.tmp/` is for session drafts/experiments, `build/scratch/` for
  measurement artifacts (bench binaries, driver output). Anything
  reproducible belongs in the repo: fixtures under
  `tests/static-reflection/`, measurement snapshots under
  `docs/static-reflection/data/` (see EVALUATION.md §5).

## 6. Doc map

- `docs/static-reflection/FEASIBILITY.md` — P2996 reflection redesign (M0–M3),
  the mirror-union route, verified patterns & pitfalls.
- `docs/static-reflection/M4_ASSESSMENT.md` — type_traits classification
  (A/B/C classes), the layered concepts strategy, composite-concept verdicts.
- `docs/static-reflection/M5_REFLECTION_TO_JSON.md` — the reflection-driven
  bidirectional serializer replacing the NLOHMANN_DEFINE_TYPE_* macro family:
  the catch-all wiring (has_to_json → is_compatible_type → constructor → CPO →
  detail::to_json chain), the exact-probe exclusion sets, the circularity
  defenses (non-circular ADL probes, in_json_namespace, adl_serializer_is_primary),
  the annotation layer (json_name/json_ignore/json_default), macro↔annotation
  semantic differences, and the verification matrix.
- Authoritative C++ feature/API facts (header map, signatures, reflection index):
  the **`modern-cpp` skill**; the local offline cppreference if present.

## 7. Commit conventions

- One logical change per commit; semantic message describing the "why".
- Keep the main-library change, the experimental-library change, and the docs in
  separate commits so history stays reviewable (see the existing 3-commit series
  on this branch).
- **Every commit on this branch carries a DCO sign-off** (`Signed-off-by:`
  line) so the history remains DCO-clean per `.github/CONTRIBUTING.md`.
  Verify with `git log --format='%h %s%n%b' -1 <sha> | grep Signed-off-by`.
- The branch is a work-in-progress experiment; commits are local until you push.
