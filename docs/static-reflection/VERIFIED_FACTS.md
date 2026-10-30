# Verified facts on this toolchain (do not re-verify)

> **When to read this.** Before writing or debugging reflection code in this
> repo. These are facts verified against *this* toolchain and *this*
> repository, so re-deriving them is wasted work. Generic C++26/`std::meta`
> facts live in the `modern-cpp` skill.
>
> Facts specific to the retired `refl2`/`rjson` experiments (catch-all
> recursion, binary code tables, `basic_json` private-layout reflection) are on
> the `legacy/reflection-experiments` branch. Do not re-derive those either —
> and do not reintroduce them here.

## Toolchain and gating

- `-freflection` defines the language macro `__cpp_impl_reflection`;
  `__cpp_lib_reflection` is a library macro supplied by libstdc++'s `<version>`
  (visible here because `json.hpp` includes `<version>` before the gate).
  Plain `-std=c++26` defines neither and `<meta>` stays empty. The header gates
  on `JSON_USE_REFLECTION` plus both P2996 macros; defining the macro without
  P2996 is a single `#error` and compiles nothing else.
- `g++-16` is required. The system `g++` on this machine is 14.2 and has no
  `<meta>`.
- `std::meta::` must be fully qualified; bare `meta::` does not compile.

## Reflection queries

- Every member/base query takes **two** arguments — `(info, access_context)` —
  with no default: `members_of`, `bases_of`, `nonstatic_data_members_of`,
  `subobjects_of`, `static_data_members_of`. GCC rejects the one-arg form.
- Queries that return a `std::vector` are **transient**: subscript or take
  `.size()` directly on the call. Binding to a local `constexpr` fails with
  "refers to a result of 'operator new'".
- `template for` exists only in range form, and the range must be an inline
  `define_static_array(...)` expression or a namespace-scope / `static
  constexpr` value.
- `if constexpr` does **not** discard the false branch; dispatch through tag
  overloads or partial specialization.
- `nonstatic_data_members_of(^^T, unprivileged())` returns the type's own
  members only. Base-class members are silently absent — a type with bases is
  rejected by the extension's `static_assert` rather than silently flattened.
- Access predicates (`is_public`, `is_private`, `is_protected`) need no context;
  `has_inaccessible_bases` / `has_inaccessible_nonstatic_data_members` are the
  dedicated "non-public present" detectors. Protected entities need
  `access_context::via(^^Derived)`.
- `bases_of` enumeration trap: an `info` taken from `bases_of(...)[i]` cannot be
  enumerated directly (`nonstatic_data_members_of` throws "not a complete class
  type", even in a `consteval` function). Recover the type first
  (`using B = typename [: type_of(b0) :];`).
- Named bit-fields serialize and round-trip: `to_json` reads them and
  `from_json` assigns directly (`value.[:member:] = ...`), so no `T&`/address is
  formed. Under `-Wconversion -Werror` that assignment is a narrowing
  conversion, exactly as hand-written `value.field = ...` would be, so the
  doctest TU does not instantiate it; the runtime fixture does. Unnamed
  bit-fields are not subobjects and are skipped by `subobjects_of`.

## Type-level annotations (P3394R4)

- Type-level annotations **are** supported on GCC 16, in the form
  `struct [[=expr]] S`. The annotation must follow the `struct`/`class`
  keyword; `[[=expr]] struct S` is ignored with a warning.
- `annotations_of(^^S)` returns them, and the annotation type is compared in the
  query domain (`remove_cvref(type_of(ann))` + `is_same_type`) — no splice
  needed, which matters because a splice requires the entity to be a constant
  expression and a `consteval` parameter is not one.
- Annotation types must be **structural AND extractable** if you need their
  values: `std::string`/`std::string_view` members are rejected ("does not have
  a structural type" — libstdc++'s members are private), a `const char*` member
  makes `meta::extract` fail, and string literals can never be template
  arguments. The retired `json_name` annotation therefore used a fixed
  `char value[64]` (structural + extractable). Bind the whole extract result,
  never a subobject of it.
- Enum/enumerator annotations, if they are ever added, must be placed after the
  identifier (`green [[=json_name{"GREEN"}]] = 2`); GCC rejects the leading form.
- Annotations on a **forward declaration** are visible to `annotations_of()`,
  so `is_annotated_v` must check completeness (`is_complete`) before querying:
  without the guard an annotated `struct S;` would select the serializer for an
  incomplete type. Locked by a `static_assert` in `unit-static_reflection.cpp`.

## What is deliberately not used

- `access_context::unchecked()` to read `basic_json::json_value` /
  `basic_json::data` (both private nested types). It is technically reachable
  through the private data members, and the retired experiment did exactly
  that — the maintainer has ruled it out. The extension uses
  `unprivileged()` and builds values through the public API.
