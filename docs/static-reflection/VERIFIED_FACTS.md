# Verified facts on this toolchain (do not re-verify)

> **When to read this.** Before writing ANY reflection/concepts/serialization
> code in this repo, and whenever a compile error looks like a compiler
> behavior question rather than a bug in your own code. This is the
> branch-specific companion to the `modern-cpp` skill (which holds the
> generic C++26/toolchain facts); this file holds the facts verified
> specifically against THIS repository's types and this branch's code.
>
> **How to add a fact.** New facts verified during implementation belong here
> (in the matching section), not in AGENTS.md — AGENTS.md is now a short
> navigation doc. Same toolchain ⇒ trust these; re-deriving them is wasted
> work.

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
- **Binary byte-code tables (M4B)** (verified by `m4_binary.cpp` differential
  vs the real library's `to_msgpack`/`to_ubjson`/`to_bson`, byte-identical):
  - The per-format primary byte-code tables (`kMsgpackCodes`/`kUbjsonCodes`/
    `kBsonCodes`) are generated from the reflection value_t enumerator set
    via the slot-index machinery (0xFF marks a range-dependent code selected
    inside the writer action — MessagePack fixnum/fixstr/fixarray/fixmap and
    8/16/32 width forms, UBJSON 'i'/'U'/'I'/'l'/'L' narrowing, BSON
    int32/int64/uint64 narrowing).
  - UBJSON negative integers must go through the SIGNED ladder: the 'U'
    (uint8) rung requires `0 <= n <= u8_max` — a plain `n <= u8_max` test
    wrongly routes -129..-33 into 'U' (real regression caught by the probe).
    Unsigned values above int64 max fall to 'H' high-precision (decimal dump
    with a length prefix) — never let them overflow an int64 conversion.
  - UBJSON floats are NOT compacted: `number_float_t` is double, so the
    prefix is always 'D' + float64 (unlike CBOR/MSGPACK's compact float32).
  - BSON element header size is `1 + name.size() + 1` (type byte + name +
    nul) — NOT `sizeof(int32) + ...`; the embedded int32 length fields are
    separate (documents, strings, arrays, binaries) and BSON numbers are
    little-endian. Top-level non-object throws (type_error 317 replicated).
  - MessagePack binary with a subtype uses ext/fixext (0xC7-0xC9, 0xD4-0xD8)
    with the subtype byte between the header and the payload; without a
    subtype it is bin8/16/32 (0xC4-0xC6).
