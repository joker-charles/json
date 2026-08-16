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
    **M4C: the mirror is gone** — `reflection_json.hpp` now HOLDS the spliced
    real `json_value` as its storage carrier (it is usable directly because:
    (1) the real union has NO user-declared destructor — trivial union, heap
    release is `data::destroy`'s manual job; (2) union members default to
    PUBLIC, so `u.object = new ...` direct member access compiles; (3)
    `json_value() = default` + `{}` value-init zeroes the first member, no
    allocation). The former "mirror avoids the real union's heap-allocation
    constructor contract" rationale does not hold — `json_value()` never
    allocates and `json_value(value_t)` is simply not used (construction is
    manual, same as before).
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
- **BSON read direction (M4B-2)** (verified by `m4b_bson_reader.cpp`
  differential vs the real library's `from_bson`, byte-identical on legal
  input):
  - The reverse table `kBsonsLoad` maps the raw element-type byte to
    `{union slot, payload kind}` — one MORE dimension than the writer
    tables, because BSON 0x10 (int32) and 0x12 (int64) both map to
    `number_integer` but carry different payload widths. Table entries are
    generated from 9 consteval specializations; unused bytes are invalid;
    completeness is static_asserted (each entry's slot matches the writer's
    `kBsonCodes` / `slot_index`).
  - BSON framing: [int32 document size][elements][0x00]; an element is
    [type byte][cstr name][0x00][payload]; strings carry [int32 len][len-1
    bytes][0x00] (the declared length INCLUDES the terminator); binaries
    [int32 len][1-byte subtype][len bytes]; array documents ignore their
    keys (position-based, matching the library); the declared document size
    must equal the bytes consumed. A minimal document {0x05,0,0,0,0} is
    LEGAL (4-byte size + 1-byte terminator) — both sides parse it as {}.
  - `byte_container_with_subtype` (json::binary_t) has NO iterator-pair
    constructor — build the container first, then the wrapper.
  - `enum class X : std::uint8_t` with `invalid = 0xFF` FIRST overflows at
    the next enumerator (255 -> 256) — put 0xFF sentinels LAST.
  - The reader's errors fail safely (false + diagnostic) but do NOT match
    the library's parse_error codes — differential coverage is legal input
    only (documented boundary).
  - The reflection serializer's dump previously rendered -0.0 as "0.0";
    fixed to match the library ("-0.0", std::signbit check).
- **M4D completed API surface** (verified by `m4d_api.cpp` differential, 1411
  checks, ASan clean; design in `docs/static-reflection/M4D_API_SURFACE.md`):
  - The value_t ordering/name tables (`kValueTNames`/`kValueTTypeNames`/
    `kValueTWeights` + `value_t_order`/`value_t_less`/`value_t_weight`) are
    generated from `enumerators_of(^^value_t)` via identifier-keyed consteval
    functions (repro_m1 route a) and REPLACE the reliance on the hand-written
    `value_t.hpp order[]`/`json.hpp type_name()` switch inside the reflection
    header. Weights: null=0, boolean=1, number_*=2, object=3, array=4,
    string=5, binary=6, discarded=-1 (unordered); the 10->8 partial mapping
    is unchanged (null/discarded have no union slot). The 10x10 pairwise
    differential vs `value_t::operator<=>`/`operator<` (incl. discarded
    unordered) is byte-exact.
  - The real library's `size()`/`empty()` semantics are NOT container-size for
    every type: null -> 0/true, array/object -> container, and EVERY other
    type (string/boolean/numbers/binary/discarded) -> 1/false.
  - `std::swap(m_value, other.m_value)` on the REAL `json_value` is a safe
    bitwise exchange: all 8 union members are trivial (pointers/scalars), so
    the implicitly-declared move ops are trivial — the real library's own
    `swap(reference)` does exactly this (json.hpp:3545).
  - `basic_json_reflection::at`/`erase` return the REAL `nlohmann::json&`
    stored in the object/array containers, so mutations through `at` write
    straight into the stored containers.
  - nlohmann exceptions derive ONLY from `std::exception` (NOT
    `std::out_of_range`/`std::runtime_error`) — differential harnesses must
    map the study library's plain std exceptions onto the nlohmann taxonomy
    by category tag, not by catch type.
  - The real `dump_float` writes "null" for NaN **and** +/-inf
    (`if (!std::isfinite(x))`) — the reflection serializer previously wrote
    "1e+999"/"-1e+999" for inf (latent drift, exposed by the M4D value
    matrix, fixed; m3_dump now pins +inf/-inf/NaN/-0.0).
  - nlohmann initializer_list nesting: `{"b", {"nested", true}}` builds "b"
    as an ARRAY `["nested", true]` (the inner list's elements are not
    arrays, so the object-pair detection fails); nested objects need the
    double-brace form `{"b", {{"nested", true}}}`.
- **UBJSON optimized modes + BJData (M4E)** (verified by `m4e_ubjson_opt.cpp`,
  630 byte-identical checks vs `to_ubjson`/`to_bjdata`, ASan clean):
  - BJData writes ALL numbers and length prefixes LITTLE-endian —
    `write_number(n, OutputIsLittleEndian=use_bjdata)` applies to signed,
    unsigned AND float payloads alike (not just the unsigned types).
  - The BJData width ladders insert 'u' (uint16) / 'm' (uint32) / 'M'
    (uint64) rungs between the signed rungs; plain UBJSON uint64 above
    int64 max falls to 'H' high-precision (decimal dump + length prefix).
  - The '$' type optimization checks `ubjson_prefix` equality (first element
    assumed same for arrays, ALL values checked for objects) and EXCLUDES
    the markers ['[' '{' 'S' 'H' 'T' 'F' 'N' 'Z'] under BJData only — plain
    UBJSON happily emits `[$S#...` etc. use_type REQUIRES use_count (the
    library JSON_ASSERTs it).
  - BJData draft3 (bjdata_version_t::draft3) differs from draft2 ONLY in the
    binary encoding: the marker is 'B' instead of 'U' and the '$' prefix is
    emitted even for an empty binary.
  - JData ndarray: an object with exactly {_ArrayType_, _ArraySize_,
    _ArrayData_} is encoded as `[$<dtype>#<size-array> <compact elements>]`
    when dtype is in the 12-entry map, every dimension is a non-negative
    integer with a non-overflowing product, the data length matches, and
    every element is the right kind (float for d/D, integer otherwise);
    otherwise it falls back to a plain object. Compact elements are always
    little-endian (write_number(..., true) hardcoded).
  - The library's public to_ubjson/to_bjdata in this tree are STATIC
    (`json::to_ubjson(j, use_count, use_type)`) with add_prefix fixed at
    write_ubjson's default (true) — no add_prefix parameter is exposed.
- **std::variant codec support (M7)** (verified by `probe_variant.cpp`, 39
  checks, ASan clean):
  - The oneof wire format is {"index": N, "value": <alternative>};
    std::monostate serializes as "value": null and is skipped on read.
  - from_json dispatches the runtime index through a compile-time
    index_sequence fold (`idx == I ? emplace_and_deserialize<B,T,I>() :
    void()`) — emplace<I>() requires default-constructible alternatives;
    out-of-range index throws type_error 302.
  - A `json` ALTERNATIVE inside a variant is NOT supported: the variant's
    template arguments carry the nlohmann namespace, so in_json_namespace
    classifies the whole type as library-internal (the M5 circularity
    defense — no way around it without weakening the probes).
  - The codec priority chain is nested json=7, variant=6, optional=5,
    adl=4, array=3, object=2, struct=1, static_assert=0 — and BOTH dispatch
    entry points (serialize_one/deserialize_one) MUST pass priority_tag<7>;
    passing <6> silently reroutes nested json values into the array-like
    branch (real regression caught by probe_adl_recursion during M7).
- **Iterators (M4D-2)** (verified by `m4d2_iterators.cpp`, 50 checks, ASan
  clean):
  - The reflection iterator mirrors iter_impl: object (map iterator) /
    array (vector iterator) / primitive (begin=0, end=1, deref only at
    begin, null begin==end) modes. Container elements are the real
    nlohmann::json values, so dereference returns json&; non-container
    values are NOT stored as json (raw scalars / string_t / binary_t in
    the union), so primitive-mode dereference materializes into an
    iterator-owned mutable json scratch (last-deref-wins aliasing —
    documented study-library limitation).
  - operator[] has the library's null -> container implicit conversion
    (null becomes an empty array/object) and the out-of-range array
    fill-up via resize(idx+1); erase(iterator) on a scalar/string/binary
    resets the value to null (destroy frees string/binary pointers);
    erase(iterator) on null/discarded throws; cross-container iterator
    comparison throws.
- **Type-surface alignment** (verified by `probe_type_alignment.cpp`, 14
  checks, ASan clean):
  - `is_compatible_binary_type<json, B::binary_t> == 0`; bare
    `std::vector<uint8_t>` IS a number array upstream (binary is the
    `byte_container_with_subtype` wrapper, which exposes `value_type` +
    `begin`/`end` and so was mis-classified array_like by refl2 — it is
    now routed to the adl branch via `is_library_dedicated_array`).
  - `std::u8string` (char8_t) is NOT string_like (char8_t does not
    construct `string_t`), so the refl2 array branch emitted a number
    array; it now routes to the library's char8_t-string overload via an
    `is_char8_string` SFINAE probe (value_type access must be guarded —
    non-string types hard-error otherwise).
  - `std::forward_list` from_json has no push_back/insert/operator[] and
    `std::valarray` from_json defaults to size 0 (fixed-size branch
    aborts); both route to the adl branch (the library's front_inserter /
    resize overloads).
  - C-array members (int[N]) were a priority-0 static_assert; the
    `!(std::is_array && !string_like)` adl exclusion was dropped so they
    reach the library's C-array overloads. 2D C arrays route recursively
    through the 1D overload.
  - `std::map<non-string key, T>` members serialize as a key-to-string
    object (`{"1":10}`) — a documented divergence from upstream's
    pair-array form (`[[1,10]]`); refl2's form round-trips, upstream's
    key-constructibility check is deliberately not mirrored (is_object_like
    stays a key_type/mapped_type existence probe).
- **concepts boolean** (verified by `concepts_smoke.cpp` across
  `-std=c++11/14/17/20/26`): `boolean_like<T,B>` must use `B::boolean_t`
  (not hard-coded `bool`, and no remove_cvref — the value param already
  drops cv/ref), mirroring `is_same<T, B::boolean_t>` exactly.
- **cold paths** (verified by `probe_negative_runtime.cpp`): struct
  from_json type-mismatch throws type_error 304 (array/string) / 302
  (wrong member type) / out_of_range 403 (missing key); json_default
  falls back only on a MISSING key, an explicit null still throws 302;
  annotated enum to_json of an out-of-table value returns the first entry.
