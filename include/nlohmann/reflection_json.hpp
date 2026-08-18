// reflection_json.hpp — a C++26 / P2996 static-reflection playground mirroring
// a slice of nlohmann::basic_json's tagged-union logic, to prove that the
// value_t <-> json_value-member <-> storage-category mapping can be driven by
// a compile-time reflection-generated table instead of hand-written switch
// dispatch.
//
// NOTE on access control (verified on g++-16 16.1.0, see probe_real_json_value.cpp):
//   basic_json::json_value and basic_json::data are PRIVATE nested types, and a
//   DIRECT scope-splice `[: ^^ json::json_value :]` fails. BUT — corrected by
//   probe_real_json_value.cpp — they ARE reachable indirectly: unchecked()
//   reflection enumerates the private data members (data via m_data, json_value
//   via data::m_value), type_of yields the private nested type, and a CONSTEVAL
//   helper can enumerate its members (inside `template for` the
//   indirectly-obtained type spuriously reports "not a complete class type" —
//   GCC 16 limitation). The storage carrier IS the real type: the spliced
//   `refl_detail::real_json_value` is held directly (M4C — the former
//   json_value_mirror union is gone). That is safe because the real union has
//   no user-declared destructor, its members are public (union default), and
//   `json_value() = default` + `{}` value-init never allocates. kStorage /
//   kMemberIds below are generated from the same real type — there is no
//   second type to drift. value_t (nlohmann::detail::value_t) is PUBLIC and is
//   reflected directly.
//
// Build/link flags: -std=c++26 -freflection ; include the repo's single_include.
// This header reuses the library's public type aliases (object_t, array_t, ...)
// via nlohmann::json but keeps its own storage, so it never touches the real
// private internals and leaves the C++11 path untouched.
//
// Design (the "single source of truth" claim):
//   * kStorage   — reflection-generated FROM THE REAL json_value: which union
//                  member is pointer-stored. This is the ONE place the
//                  pointer/scalar split lives; the real library repeats it
//                  across ctor/destroy/invariant.
//   * slot_index<V> — compile-time map value_t -> union member index; a value_t
//                  without a slot (null/discarded) has has==false.
//   * default-construct/destroy are dispatched by a `template for` over ALL
//                  value_t enumerators, so adding a value_t without wiring it
//                  is a COMPILE ERROR, never a silent drift.
//
// Binary formats (M3 + M4B + M4B-2): the value_t -> byte-code mapping of
// every format is a reflection-generated consteval table (kMsgpackCodes /
// kUbjsonCodes / kBsonCodes); writers dispatch by `template for` over
// kValueTInfos with per-enumerator actions, readers (currently only BSON,
// reflection_bson_parser) dispatch through the reverse kBsonsLoad table
// (byte code -> {union slot, payload kind}). Byte-level behavior is
// differential-tested byte-identical against the library (m3_binary.cpp /
// m4_binary.cpp / m4b_bson_reader.cpp).

#pragma once

#include <algorithm>   // reverse
#include <array>
#include <cmath>       // isfinite, isinf, isnan, abs
#include <compare>     // partial_ordering (value_t ordering table)
#include <cstddef>     // ptrdiff_t (erase index arithmetic)
#include <cstdint>
#include <cstdio>      // snprintf
#include <cstdlib>     // abort
#include <cstring>     // memcpy
#include <iterator>    // reverse_iterator, advance, next (M4D-2 iterators)
#include <limits>      // numeric_limits
#include <stdexcept>   // runtime_error, out_of_range (BSON check, at/erase bounds)
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <meta>

namespace rjson
{
using json   = nlohmann::json;
using value_t = nlohmann::detail::value_t;

// ---------------------------------------------------------------------------
// STORAGE = the REAL basic_json::json_value, spliced out of the private
// nested type via type_of (probe_real_json_value.cpp verified pattern — the
// M4C upgrade; the former json_value_mirror union is gone). The real union
// is usable directly as a storage carrier because (verified on this
// toolchain):
//   * it has NO user-declared destructor (trivial union; heap release is
//     data::destroy's manual job in the real library) — default-construct
//     never allocates;
//   * its members are PUBLIC (union members default to public), so
//     `u.object = new ...` direct member access compiles;
//   * `json_value() = default` + `{}` value-initialization zeroes the first
//     member (the object pointer) — same semantics as the old mirror.
// The SCHEMA below (kStorage / kMemberIds — names + storage category) is
// generated from this same real type, so there is no second type to drift.
// ---------------------------------------------------------------------------

namespace refl_detail
{
// --- reach the REAL basic_json::json_value (private nested union) ---
// Indirect route verified by probe_real_json_value.cpp: unchecked() reflection
// finds basic_json's private data members; data is [0] (m_data), and inside it
// m_value is [1]; type_of yields the private nested union. Enumeration of its
// members works ONLY from a consteval context (template-for inline reports
// "not a complete class type" — GCC 16 limitation).
consteval std::meta::info real_data_info()
{
    constexpr auto m_data = std::meta::nonstatic_data_members_of(
        ^^json, std::meta::access_context::unchecked())[0];
    return std::meta::type_of(m_data);
}

consteval std::meta::info real_json_value_info()
{
    constexpr auto m_value = std::meta::nonstatic_data_members_of(
        real_data_info(), std::meta::access_context::unchecked())[1];
    return std::meta::type_of(m_value);
}

// splice the REAL private nested union out and use it as the storage type
using real_json_value = typename [: real_json_value_info() :];

consteval std::size_t member_count()
{
    return std::meta::nonstatic_data_members_of(
        real_json_value_info(), std::meta::access_context::unchecked()).size();
}

template<std::size_t I>
consteval bool member_is_pointer()
{
    constexpr auto m = std::meta::nonstatic_data_members_of(
        real_json_value_info(), std::meta::access_context::unchecked())[I];
    using M = typename [: std::meta::type_of(m) :];
    return std::is_pointer_v<M>;
}

template<std::size_t... I>
consteval auto storage_impl(std::index_sequence<I...>)
{
    return std::array<bool, sizeof...(I)> { member_is_pointer<I>()... };
}
consteval auto storage_category()
{
    return storage_impl(std::make_index_sequence<member_count()> {});
}

template<std::size_t... I>
consteval auto ids_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)>
    {
        std::meta::identifier_of(std::meta::nonstatic_data_members_of(
                                     real_json_value_info(), std::meta::access_context::unchecked())[I])...
    };
}
consteval auto member_ids()
{
    return ids_impl(std::make_index_sequence<member_count()> {});
}
} // namespace refl_detail

// Reflection-generated tables (single source of truth for the union mapping).
constexpr std::size_t kMemberCount = refl_detail::member_count();
constexpr auto        kStorage     = refl_detail::storage_category(); // index -> is_pointer
constexpr auto        kMemberIds   = refl_detail::member_ids();      // index -> name

// ---------------------------------------------------------------------------
// value_t -> union member index. null and discarded have NO slot. The
// `template for`-driven dispatcher (see construct_default<T>/destroy<T> below)
// only handles enumerators; any value_t without a slot_index<> specialization
// fails to compile — the completeness guarantee.
// ---------------------------------------------------------------------------
template<value_t V> struct slot_index
{
    static constexpr bool has = false;
};
template<> struct slot_index<value_t::object>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 0;
};
template<> struct slot_index<value_t::array>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 1;
};
template<> struct slot_index<value_t::string>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 2;
};
template<> struct slot_index<value_t::binary>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 3;
};
template<> struct slot_index<value_t::boolean>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 4;
};
template<> struct slot_index<value_t::number_integer>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 5;
};
template<> struct slot_index<value_t::number_unsigned>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 6;
};
template<> struct slot_index<value_t::number_float>
{
    static constexpr bool has = true;
    static constexpr std::size_t value = 7;
};

// ---------------------------------------------------------------------------
// Compile-time dispatch over ALL value_t enumerators.
// For each enumerator V:
//   - if it has a slot, the active member is set to its default;
//   - else (null/discarded) the union stays zeroed.
// This replaces the hand-written 10-way default-construction switch with a
// reflection-driven instantiation that is complete by construction.
// ---------------------------------------------------------------------------
// The set of enumerators, captured as a namespace-scope static array so a
// range-based `template for` can iterate it (transient-vector rule).
constexpr auto kValueTInfos =
    std::define_static_array(std::meta::enumerators_of(^^value_t));

// ---------------------------------------------------------------------------
// value_t name & ordering tables (reflection-generated; route a — identifier-
// keyed consteval functions over the enumerator set, the repro_m1.cpp pattern
// landed in the real header). These stand in for the library's hand-written
// value_t.hpp order[] (weights) and json.hpp type_name() switch (display
// names) inside THIS library: type ordering and type names are generated
// from the enumerator set, so a new value_t enumerator shows up in the tables
// automatically. The 10->8 partial mapping (null/discarded have no union
// slot) is unchanged — discarded simply has no comparable weight.
// ---------------------------------------------------------------------------
namespace refl_detail
{
// identifier of the I-th value_t enumerator (transient-vector rule: subscript
// the call directly, never bind the vector to a local constexpr)
consteval std::string_view value_t_id_at(std::size_t i)
{
    return std::meta::identifier_of(std::meta::enumerators_of(^^value_t)[i]);
}

// display name with json.hpp type_name() semantics: the three number types
// are all reported as "number"; every other type is its own identifier.
consteval std::string_view value_t_display_name(std::string_view id)
{
    if (id == "number_integer" || id == "number_unsigned" || id == "number_float")
    {
        return "number";
    }
    return id;
}

// Python-like sort weight mirroring value_t.hpp order[]:
//   null=0, boolean=1, number_*=2, object=3, array=4, string=5, binary=6,
//   discarded is NOT comparable -> sentinel -1 (unordered).
consteval int value_t_sort_weight(std::string_view id)
{
    if (id == "null")
    {
        return 0;
    }
    if (id == "boolean")
    {
        return 1;
    }
    if (id == "number_integer" || id == "number_unsigned" || id == "number_float")
    {
        return 2;
    }
    if (id == "object")
    {
        return 3;
    }
    if (id == "array")
    {
        return 4;
    }
    if (id == "string")
    {
        return 5;
    }
    if (id == "binary")
    {
        return 6;
    }
    return -1; // discarded
}

template<std::size_t... I>
consteval auto value_t_names_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)> { value_t_id_at(I)... };
}
template<std::size_t... I>
consteval auto value_t_type_names_impl(std::index_sequence<I...>)
{
    return std::array<std::string_view, sizeof...(I)> { value_t_display_name(value_t_id_at(I))... };
}
template<std::size_t... I>
consteval auto value_t_weights_impl(std::index_sequence<I...>)
{
    return std::array<int, sizeof...(I)> { value_t_sort_weight(value_t_id_at(I))... };
}
} // namespace refl_detail

// three parallel tables over the enumerator set, in enumeration order
constexpr auto kValueTNames     = refl_detail::value_t_names_impl(
    std::make_index_sequence<kValueTInfos.size()> {});
constexpr auto kValueTTypeNames = refl_detail::value_t_type_names_impl(
    std::make_index_sequence<kValueTInfos.size()> {});
constexpr auto kValueTWeights   = refl_detail::value_t_weights_impl(
    std::make_index_sequence<kValueTInfos.size()> {});

// the tables must cover all enumerators — a value_t added without making it
// into the tables is a compile error, never a silent ordering/name drift
static_assert(kValueTNames.size() == 10 && kValueTTypeNames.size() == 10 &&
              kValueTWeights.size() == 10,
              "value_t name/weight tables must cover all 10 enumerators");
// weights pinned to the hand-written value_t.hpp order[] (index = enumeration
// order: null=0, object=1, array=2, string=3, boolean=4, number_integer=5,
// number_unsigned=6, number_float=7, binary=8, discarded=9)
static_assert(kValueTWeights[0] == 0 &&   // null
              kValueTWeights[1] == 3 &&   // object
              kValueTWeights[2] == 4 &&   // array
              kValueTWeights[3] == 5 &&   // string
              kValueTWeights[4] == 1 &&   // boolean
              kValueTWeights[5] == 2 &&   // number_integer
              kValueTWeights[6] == 2 &&   // number_unsigned
              kValueTWeights[7] == 2 &&   // number_float
              kValueTWeights[8] == 6 &&   // binary
              kValueTWeights[9] == -1,    // discarded -> unordered
              "value_t weights must match value_t.hpp order[]");
// display names pinned to json.hpp type_name()
static_assert(kValueTTypeNames[0] == "null" &&
              kValueTTypeNames[1] == "object" &&
              kValueTTypeNames[2] == "array" &&
              kValueTTypeNames[3] == "string" &&
              kValueTTypeNames[4] == "boolean" &&
              kValueTTypeNames[5] == "number" &&
              kValueTTypeNames[6] == "number" &&
              kValueTTypeNames[7] == "number" &&
              kValueTTypeNames[8] == "binary" &&
              kValueTTypeNames[9] == "discarded",
              "value_t display names must match json.hpp type_name()");

namespace refl_detail
{
// weight of a value_t (-1 = discarded, unordered); table-driven
[[nodiscard]] constexpr int value_t_weight(const value_t t) noexcept
{
    const auto idx = static_cast<std::size_t>(t);
    return idx < kValueTWeights.size() ? kValueTWeights[idx] : -1;
}

// type ordering, replacing the hand-written value_t.hpp order[] comparison:
// equal weights are equivalent; discarded (weight -1) is unordered — the
// same partial_ordering semantics as value_t::operator<=>.
[[nodiscard]] constexpr std::partial_ordering value_t_order(const value_t lhs,
                                                             const value_t rhs) noexcept
{
    const int lw = value_t_weight(lhs);
    const int rw = value_t_weight(rhs);
    if (lw < 0 || rw < 0)
    {
        return std::partial_ordering::unordered;
    }
    return lw <=> rw;
}

// same semantics as value_t::operator< (is_lt): discarded is never less
[[nodiscard]] constexpr bool value_t_less(const value_t lhs, const value_t rhs) noexcept
{
    return value_t_order(lhs, rhs) == std::partial_ordering::less;
}
} // namespace refl_detail

// Per-enumerator construct/destroy actions, selected by tag dispatch to keep
// GCC 16's `if constexpr` false-branch problem out of the picture.
template<value_t V>
consteval std::size_t slot_of()            // 0..7 for storage, else SIZE_MAX
{
    if constexpr (slot_index<V>::has)
    {
        return slot_index<V>::value;
    }
    else
    {
        return static_cast<std::size_t>(-1);
    }
}

// --- construction action for one enumerator (compile-time V) ---
template<value_t V>
void construct_one(refl_detail::real_json_value& u)
{
    if constexpr (V == value_t::object)
    {
        u.object  = new json::object_t();
    }
    else if constexpr (V == value_t::array)
    {
        u.array   = new json::array_t();
    }
    else if constexpr (V == value_t::string)
    {
        u.string  = new json::string_t();
    }
    else if constexpr (V == value_t::binary)
    {
        u.binary  = new json::binary_t();
    }
    else if constexpr (V == value_t::boolean)
    {
        u.boolean = false;
    }
    else if constexpr (V == value_t::number_integer)
    {
        u.number_integer = 0;
    }
    else if constexpr (V == value_t::number_unsigned)
    {
        u.number_unsigned = 0;
    }
    else if constexpr (V == value_t::number_float)
    {
        u.number_float = 0.0;
    }
    // null / discarded: no slot -> do nothing (keep union zeroed)
}

// --- destruction action for one enumerator (compile-time V) ---
template<value_t V>
void destroy_one(refl_detail::real_json_value& u)
{
    if constexpr (V == value_t::object)
    {
        delete u.object;
        u.object  = nullptr;
    }
    else if constexpr (V == value_t::array)
    {
        delete u.array;
        u.array   = nullptr;
    }
    else if constexpr (V == value_t::string)
    {
        delete u.string;
        u.string  = nullptr;
    }
    else if constexpr (V == value_t::binary)
    {
        delete u.binary;
        u.binary  = nullptr;
    }
    // scalars and null/discarded: nothing to release
}

// ---------------------------------------------------------------------------
// Restricted tagged union mirroring the storage semantics.
// ---------------------------------------------------------------------------
// M4D-2: the iterator class is defined AFTER basic_json_reflection (it needs
// the complete storage type); the class itself only needs the declaration.
template<bool IsConst> class reflection_iterator;

struct basic_json_reflection
{
    template<bool> friend class reflection_iterator;

    value_t m_type = value_t::null;
    refl_detail::real_json_value m_value{}; // value-init zeroes member [0] (object ptr)

    basic_json_reflection() = default;

    explicit basic_json_reflection(const value_t t) : m_type(t)
    {
        // instantiate the constructor for THIS type (complete by construction)
        construct_for(m_type);
    }

    ~basic_json_reflection()
    {
        destroy();
    }
    basic_json_reflection(const basic_json_reflection&) = delete;
    basic_json_reflection& operator=(const basic_json_reflection&) = delete;

    [[nodiscard]] bool is_object()  const
    {
        return m_type == value_t::object;
    }
    [[nodiscard]] bool is_array()   const
    {
        return m_type == value_t::array;
    }
    [[nodiscard]] bool is_string()  const
    {
        return m_type == value_t::string;
    }
    [[nodiscard]] bool is_binary()  const
    {
        return m_type == value_t::binary;
    }
    [[nodiscard]] bool is_boolean() const
    {
        return m_type == value_t::boolean;
    }
    [[nodiscard]] bool is_number_integer()  const
    {
        return m_type == value_t::number_integer || m_type == value_t::number_unsigned;
    }
    [[nodiscard]] bool is_number_unsigned() const
    {
        return m_type == value_t::number_unsigned;
    }
    [[nodiscard]] bool is_number_float()    const
    {
        return m_type == value_t::number_float;
    }
    [[nodiscard]] bool is_number() const
    {
        return is_number_integer() || is_number_float();
    }
    [[nodiscard]] bool is_null()      const
    {
        return m_type == value_t::null;
    }
    [[nodiscard]] bool is_discarded() const
    {
        return m_type == value_t::discarded;
    }

    // Is the storage for the current type a heap pointer? Table-driven (no
    // hand-written pointer list — answered from kStorage + slot_index).
    [[nodiscard]] bool current_storage_is_pointer() const noexcept
    {
        return storage_pointer_for(m_type);
    }

    // --- deep copy from a real nlohmann::json value (for differential
    // serialization; mirror members reuse the same container/member types) ---
    basic_json_reflection& assign_from(const json& src)
    {
        destroy();                       // release current contents
        // infer the type from the public predicate set, then construct the
        // active member and deep-copy the payload
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (src_has_type(src, V))
            {
                m_type = V;
                construct_one<V>(m_value);
                copy_from_lib<V>(src);
            }
        }
        return *this;
    }

    // --- accessors for the serializer (friend) ---
    [[nodiscard]] value_t type() const noexcept
    {
        return m_type;
    }
    [[nodiscard]] const refl_detail::real_json_value& value() const noexcept
    {
        return m_value;
    }

  private:
    static bool src_has_type(const json& src, const value_t v)
    {
        switch (v) // NOLINT — public predicate dispatch (not the drift source)
        {
            case value_t::object:
                return src.is_object();
            case value_t::array:
                return src.is_array();
            case value_t::string:
                return src.is_string();
            case value_t::binary:
                return src.is_binary();
            case value_t::boolean:
                return src.is_boolean();
            case value_t::number_integer:
                return src.is_number_integer() && !src.is_number_unsigned();
            case value_t::number_unsigned:
                return src.is_number_unsigned();
            case value_t::number_float:
                return src.is_number_float();
            case value_t::null:
                return src.is_null();
            case value_t::discarded:
                return src.is_discarded();
        }
        return false;
    }

    template<value_t V>
    void copy_from_lib(const json& src)
    {
        if constexpr (V == value_t::object)
        {
            *m_value.object = src.get_ref<const json::object_t&>();
        }
        else if constexpr (V == value_t::array)
        {
            *m_value.array = src.get_ref<const json::array_t&>();
        }
        else if constexpr (V == value_t::string)
        {
            *m_value.string = src.get_ref<const json::string_t&>();
        }
        else if constexpr (V == value_t::binary)
        {
            *m_value.binary = src.get_ref<const json::binary_t&>();
        }
        else if constexpr (V == value_t::boolean)
        {
            m_value.boolean = src.get<json::boolean_t>();
        }
        else if constexpr (V == value_t::number_integer)
        {
            m_value.number_integer = src.get<json::number_integer_t>();
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            m_value.number_unsigned = src.get<json::number_unsigned_t>();
        }
        else if constexpr (V == value_t::number_float)
        {
            m_value.number_float = src.get<json::number_float_t>();
        }
        // null / discarded: no payload to copy
    }

    void construct_for(const value_t t)
    {
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                construct_one<V>(m_value);
            }
        }
    }

    void destroy()
    {
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (m_type == V)
            {
                destroy_one<V>(m_value);
            }
        }
        m_type = value_t::null;
    }

    static bool storage_pointer_for(const value_t t) noexcept
    {
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                if constexpr (!slot_index<V>::has)
                {
                    return false;
                }
                else
                {
                    return kStorage[slot_index<V>::value];
                }
            }
        }
        return false;
    }

    // ---- M4D: per-enumerator clear action (table-driven over the
    // enumerator set, same shape as construct_one/destroy_one) ----
    template<value_t V>
    void clear_one()
    {
        if constexpr (V == value_t::number_integer)
        {
            m_value.number_integer = 0;
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            m_value.number_unsigned = 0;
        }
        else if constexpr (V == value_t::number_float)
        {
            m_value.number_float = 0.0;
        }
        else if constexpr (V == value_t::boolean)
        {
            m_value.boolean = false;
        }
        else if constexpr (V == value_t::string)
        {
            m_value.string->clear();
        }
        else if constexpr (V == value_t::binary)
        {
            m_value.binary->clear();
        }
        else if constexpr (V == value_t::array)
        {
            m_value.array->clear();
        }
        else if constexpr (V == value_t::object)
        {
            m_value.object->clear();
        }
        // null / discarded: nothing to clear (library's default: break)
    }

    // ---- M4D: comparison cores (replicate the library's
    // JSON_IMPLEMENT_OPERATOR semantics; the non-legacy
    // JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON mode — the default — is the
    // one replicated here) ----
    // true if the pair is unordered: NaN number vs any number, or any discarded
    static bool compares_unordered(const basic_json_reflection& lhs,
                                   const basic_json_reflection& rhs) noexcept
    {
        if ((lhs.is_number_float() && std::isnan(lhs.m_value.number_float) && rhs.is_number())
                || (rhs.is_number_float() && std::isnan(rhs.m_value.number_float) && lhs.is_number()))
        {
            return true;
        }
        return lhs.is_discarded() || rhs.is_discarded();
    }

    static bool equal_core(const basic_json_reflection& lhs,
                           const basic_json_reflection& rhs) noexcept
    {
        const value_t l = lhs.m_type;
        const value_t r = rhs.m_type;
        if (l == r)
        {
            switch (l)
            {
                case value_t::array:
                    return *lhs.m_value.array == *rhs.m_value.array;
                case value_t::object:
                    return *lhs.m_value.object == *rhs.m_value.object;
                case value_t::null:
                    return true;
                case value_t::string:
                    return *lhs.m_value.string == *rhs.m_value.string;
                case value_t::boolean:
                    return lhs.m_value.boolean == rhs.m_value.boolean;
                case value_t::number_integer:
                    return lhs.m_value.number_integer == rhs.m_value.number_integer;
                case value_t::number_unsigned:
                    return lhs.m_value.number_unsigned == rhs.m_value.number_unsigned;
                case value_t::number_float:
                    return lhs.m_value.number_float == rhs.m_value.number_float; // *NOPAD* float-equal is intended
                case value_t::binary:
                    return *lhs.m_value.binary == *rhs.m_value.binary;
                case value_t::discarded:
                default:
                    return false;
            }
        }
        if (l == value_t::number_integer && r == value_t::number_float)
        {
            return static_cast<json::number_float_t>(lhs.m_value.number_integer) == rhs.m_value.number_float;
        }
        if (l == value_t::number_float && r == value_t::number_integer)
        {
            return lhs.m_value.number_float == static_cast<json::number_float_t>(rhs.m_value.number_integer);
        }
        if (l == value_t::number_unsigned && r == value_t::number_float)
        {
            return static_cast<json::number_float_t>(lhs.m_value.number_unsigned) == rhs.m_value.number_float;
        }
        if (l == value_t::number_float && r == value_t::number_unsigned)
        {
            return lhs.m_value.number_float == static_cast<json::number_float_t>(rhs.m_value.number_unsigned);
        }
        if (l == value_t::number_unsigned && r == value_t::number_integer)
        {
            // negative signed < any unsigned: preserve the ordering relationship
            return (rhs.m_value.number_integer < 0)
                   ? (static_cast<json::number_integer_t>(1) == static_cast<json::number_integer_t>(-1))
                   : (lhs.m_value.number_unsigned == static_cast<json::number_unsigned_t>(rhs.m_value.number_integer));
        }
        if (l == value_t::number_integer && r == value_t::number_unsigned)
        {
            return (lhs.m_value.number_integer < 0)
                   ? (static_cast<json::number_integer_t>(-1) == static_cast<json::number_integer_t>(1))
                   : (static_cast<json::number_unsigned_t>(lhs.m_value.number_integer) == rhs.m_value.number_unsigned);
        }
        return false; // default_result for ==
    }

    static bool less_core(const basic_json_reflection& lhs,
                          const basic_json_reflection& rhs) noexcept
    {
        const value_t l = lhs.m_type;
        const value_t r = rhs.m_type;
        if (l == r)
        {
            switch (l)
            {
                case value_t::array:
                    return *lhs.m_value.array < *rhs.m_value.array;
                case value_t::object:
                    return *lhs.m_value.object < *rhs.m_value.object;
                case value_t::null:
                    return false;
                case value_t::string:
                    return *lhs.m_value.string < *rhs.m_value.string;
                case value_t::boolean:
                    return lhs.m_value.boolean < rhs.m_value.boolean;
                case value_t::number_integer:
                    return lhs.m_value.number_integer < rhs.m_value.number_integer;
                case value_t::number_unsigned:
                    return lhs.m_value.number_unsigned < rhs.m_value.number_unsigned;
                case value_t::number_float:
                    return lhs.m_value.number_float < rhs.m_value.number_float;
                case value_t::binary:
                    return *lhs.m_value.binary < *rhs.m_value.binary;
                case value_t::discarded:
                default:
                    return false;
            }
        }
        if (l == value_t::number_integer && r == value_t::number_float)
        {
            return static_cast<json::number_float_t>(lhs.m_value.number_integer) < rhs.m_value.number_float;
        }
        if (l == value_t::number_float && r == value_t::number_integer)
        {
            return lhs.m_value.number_float < static_cast<json::number_float_t>(rhs.m_value.number_integer);
        }
        if (l == value_t::number_unsigned && r == value_t::number_float)
        {
            return static_cast<json::number_float_t>(lhs.m_value.number_unsigned) < rhs.m_value.number_float;
        }
        if (l == value_t::number_float && r == value_t::number_unsigned)
        {
            return lhs.m_value.number_float < static_cast<json::number_float_t>(rhs.m_value.number_unsigned);
        }
        if (l == value_t::number_unsigned && r == value_t::number_integer)
        {
            return (rhs.m_value.number_integer < 0)
                   ? (static_cast<json::number_integer_t>(1) < static_cast<json::number_integer_t>(-1))
                   : (lhs.m_value.number_unsigned < static_cast<json::number_unsigned_t>(rhs.m_value.number_integer));
        }
        if (l == value_t::number_integer && r == value_t::number_unsigned)
        {
            return (lhs.m_value.number_integer < 0)
                   ? (static_cast<json::number_integer_t>(-1) < static_cast<json::number_integer_t>(1))
                   : (static_cast<json::number_unsigned_t>(lhs.m_value.number_integer) < rhs.m_value.number_unsigned);
        }
        if (compares_unordered(lhs, rhs))
        {
            return false;
        }
        return refl_detail::value_t_less(l, r); // reflection-generated ordering
    }

  public:
    // ---- M4D: completed tagged-union API surface ----

    // --- type name (reflection-generated display table; replaces the
    // hand-written type_name() switch) ---
    [[nodiscard]] const char* type_name() const noexcept
    {
        const auto idx = static_cast<std::size_t>(m_type);
        return idx < kValueTTypeNames.size() ? kValueTTypeNames[idx].data() : "invalid";
    }

    // --- size / empty (library semantics: containers report their element
    // count, null is empty, every other type counts as a single element) ---
    [[nodiscard]] std::size_t size() const noexcept
    {
        if (is_null())
        {
            return 0;
        }
        if (is_array())
        {
            return m_value.array->size();
        }
        if (is_object())
        {
            return m_value.object->size();
        }
        return 1; // string/boolean/numbers/binary/discarded
    }

    [[nodiscard]] bool empty() const noexcept
    {
        if (is_null())
        {
            return true;
        }
        if (is_array())
        {
            return m_value.array->empty();
        }
        if (is_object())
        {
            return m_value.object->empty();
        }
        return false; // string/boolean/numbers/binary/discarded
    }

    // --- element access with bounds checking (at) ---
    [[nodiscard]] json& at(const std::size_t idx)
    {
        if (!is_array())
        {
            throw std::runtime_error(std::string("cannot use at() with ") + type_name());
        }
        return m_value.array->at(idx); // throws std::out_of_range out of bounds
    }

    [[nodiscard]] const json& at(const std::size_t idx) const
    {
        if (!is_array())
        {
            throw std::runtime_error(std::string("cannot use at() with ") + type_name());
        }
        return m_value.array->at(idx);
    }

    [[nodiscard]] json& at(const json::object_t::key_type& key)
    {
        if (!is_object())
        {
            throw std::runtime_error(std::string("cannot use at() with ") + type_name());
        }
        const auto it = m_value.object->find(key);
        if (it == m_value.object->end())
        {
            throw std::out_of_range("key '" + key + "' not found");
        }
        return it->second;
    }

    [[nodiscard]] const json& at(const json::object_t::key_type& key) const
    {
        if (!is_object())
        {
            throw std::runtime_error(std::string("cannot use at() with ") + type_name());
        }
        const auto it = m_value.object->find(key);
        if (it == m_value.object->end())
        {
            throw std::out_of_range("key '" + key + "' not found");
        }
        return it->second;
    }

    // --- erase (key form for objects, index form for arrays; the iterator
    // forms await the iterators milestone) ---
    std::size_t erase(const json::object_t::key_type& key)
    {
        if (!is_object())
        {
            throw std::runtime_error(std::string("cannot use erase() with ") + type_name());
        }
        return m_value.object->erase(key);
    }

    void erase(const std::size_t idx)
    {
        if (!is_array())
        {
            throw std::runtime_error(std::string("cannot use erase() with ") + type_name());
        }
        if (idx >= size())
        {
            throw std::out_of_range("array index " + std::to_string(idx) + " is out of range");
        }
        m_value.array->erase(m_value.array->begin() + static_cast<std::ptrdiff_t>(idx));
    }

    // --- clear (table-driven: per-enumerator action over the enumerator set,
    // so a new value_t shows up here exactly like in construct/destroy) ---
    void clear() noexcept
    {
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (m_type == V)
            {
                clear_one<V>();
            }
        }
    }

    // --- swap ---
    void swap(basic_json_reflection& other) noexcept
    {
        // the real json_value has only trivial members, so swapping the union
        // is a bitwise exchange — the same operation the real library's swap
        // performs on its own json_value
        std::swap(m_type, other.m_type);
        std::swap(m_value, other.m_value);
    }

    friend void swap(basic_json_reflection& left, basic_json_reflection& right) noexcept
    {
        left.swap(right);
    }

    void swap(json::array_t& other)
    {
        if (!is_array())
        {
            throw std::runtime_error(std::string("cannot use swap(array_t&) with ") + type_name());
        }
        using std::swap;
        swap(*m_value.array, other);
    }

    void swap(json::object_t& other)
    {
        if (!is_object())
        {
            throw std::runtime_error(std::string("cannot use swap(object_t&) with ") + type_name());
        }
        using std::swap;
        swap(*m_value.object, other);
    }

    void swap(json::string_t& other)
    {
        if (!is_string())
        {
            throw std::runtime_error(std::string("cannot use swap(string_t&) with ") + type_name());
        }
        using std::swap;
        swap(*m_value.string, other);
    }

    void swap(json::binary_t& other)
    {
        if (!is_binary())
        {
            throw std::runtime_error(std::string("cannot use swap(binary_t&) with ") + type_name());
        }
        using std::swap;
        swap(*m_value.binary, other);
    }

    void swap(json::binary_t::container_type& other)
    {
        if (!is_binary())
        {
            throw std::runtime_error(std::string("cannot use swap(binary_t::container_type&) with ") + type_name());
        }
        using std::swap;
        swap(*m_value.binary, other);
    }

    // --- lexicographical comparison operators (friend; replicate the
    // library's observable semantics incl. NaN/discarded unordered, with the
    // reflection-generated value_t weight table as the cross-type ordering) ---
    friend bool operator==(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        return equal_core(lhs, rhs);
    }
    friend bool operator!=(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        return !(lhs == rhs);
    }
    friend bool operator<(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        return less_core(lhs, rhs);
    }
    friend bool operator<=(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        // the library passes inverse=true here, which only matters under
        // JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON (not replicated)
        if (compares_unordered(lhs, rhs))
        {
            return false;
        }
        return !(rhs < lhs);
    }
    friend bool operator>(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        if (compares_unordered(lhs, rhs))
        {
            return false;
        }
        return !(lhs <= rhs);
    }
    friend bool operator>=(const basic_json_reflection& lhs, const basic_json_reflection& rhs) noexcept
    {
        if (compares_unordered(lhs, rhs))
        {
            return false;
        }
        return !(lhs < rhs);
    }

    // ---- iterators (M4D-2) ------------------------------------------------
    // Defined out of line after reflection_iterator (which needs this class
    // complete; this class needs only the forward declaration).
    using iterator = reflection_iterator<false>;
    using const_iterator = reflection_iterator<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    iterator begin() noexcept;
    iterator end() noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;
    const_iterator cbegin() const noexcept;
    const_iterator cend() const noexcept;
    reverse_iterator rbegin() noexcept;
    reverse_iterator rend() noexcept;
    const_reverse_iterator rbegin() const noexcept;
    const_reverse_iterator rend() const noexcept;
    const_reverse_iterator crbegin() const noexcept;
    const_reverse_iterator crend() const noexcept;

    // --- element access via operator[] (null implicitly converts, exactly
    // like the library: null -> array/object, then index/key access) ---
    json& operator[](const std::size_t idx);
    json& operator[](const json::object_t::key_type& key);

    // --- lookup ---
    iterator find(const json::object_t::key_type& key) noexcept;
    bool contains(const json::object_t::key_type& key) const noexcept;
    std::size_t count(const json::object_t::key_type& key) const noexcept;

    // --- erase with iterators (the M4D iterator forms) ---
    iterator erase(iterator pos);
    iterator erase(iterator first, iterator last);
};

// ---------------------------------------------------------------------------
// Iterator over the tagged union (M4D-2).
//
// Mirrors iter_impl.hpp: three modes — object (std::map iterator), array
// (std::vector iterator), and primitive (a 0/1 begin/end index for scalar,
// string, binary, null, discarded values — primitive_iterator_t semantics:
// begin=0, end=1, dereference only at begin). The container ELEMENTS are the
// real nlohmann::json values stored in the object/array containers, so
// dereferencing returns json& (const-qualified per IsConst) exactly like the
// library. Non-container values are NOT stored as json — the reflection holds
// raw scalars / string_t / binary_t in the union — so a primitive-mode
// dereference materializes the value into an iterator-owned scratch json
// (documented aliasing limitation of this study library: the last
// dereference wins; the real library dereferences the value in place).
// The 12 hand-written switches of iter_impl.hpp are type-routed through the
// is_* predicate set / mode_for, and the member-level erase/operator[]
// dispatch uses the same template-for-over-kValueTInfos pattern as
// construct/destroy/clear.
// ---------------------------------------------------------------------------
template<bool IsConst>
class reflection_iterator
{
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = json;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<IsConst, const json*, json*>;
    using reference = std::conditional_t<IsConst, const json&, json&>;

  private:
    using reflection_t = std::conditional_t<IsConst, const basic_json_reflection, basic_json_reflection>;
    using object_iterator_t = std::conditional_t<IsConst,
        typename json::object_t::const_iterator, typename json::object_t::iterator>;
    using array_iterator_t = std::conditional_t<IsConst,
        typename json::array_t::const_iterator, typename json::array_t::iterator>;

    friend class basic_json_reflection;

    reflection_t* m_object = nullptr;
    std::uint8_t m_mode = 0;        // 0 = primitive, 1 = object, 2 = array
    object_iterator_t m_object_it{};
    array_iterator_t m_array_it{};
    std::ptrdiff_t m_primitive = 0; // primitive_iterator_t: 0 = begin, 1 = end
    mutable json m_scratch;         // materialized primitive/string/binary value

    // the mode of a value's current type (replaces iter_impl's switch)
    static std::uint8_t mode_for(const basic_json_reflection& j) noexcept
    {
        if (j.is_object())
        {
            return 1;
        }
        if (j.is_array())
        {
            return 2;
        }
        return 0;
    }

  public:
    reflection_iterator() = default;

    explicit reflection_iterator(reflection_t* object) noexcept
        : m_object(object), m_mode(mode_for(*object))
    {}

    void set_begin() noexcept
    {
        if (m_mode == 1)
        {
            m_object_it = m_object->m_value.object->begin();
        }
        else if (m_mode == 2)
        {
            m_array_it = m_object->m_value.array->begin();
        }
        else
        {
            // null is empty: begin == end; every other primitive has one element
            m_primitive = m_object->is_null() ? 1 : 0;
        }
    }

    void set_end() noexcept
    {
        if (m_mode == 1)
        {
            m_object_it = m_object->m_value.object->end();
        }
        else if (m_mode == 2)
        {
            m_array_it = m_object->m_value.array->end();
        }
        else
        {
            m_primitive = 1;
        }
    }

    reference operator*() const
    {
        if (m_mode == 1)
        {
            return m_object_it->second;
        }
        if (m_mode == 2)
        {
            return *m_array_it;
        }
        // primitive/string/binary: the library returns *m_object (a real
        // json); the reflection stores raw scalars, so synthesize the value
        // into the iterator-owned scratch (aliasing: last deref wins)
        if (m_primitive != 0)
        {
            throw std::runtime_error("reflection_iterator: cannot get value");
        }
        const value_t t = m_object->type();
        if (t == value_t::boolean)
        {
            m_scratch = m_object->m_value.boolean;
        }
        else if (t == value_t::number_integer)
        {
            m_scratch = m_object->m_value.number_integer;
        }
        else if (t == value_t::number_unsigned)
        {
            m_scratch = m_object->m_value.number_unsigned;
        }
        else if (t == value_t::number_float)
        {
            m_scratch = m_object->m_value.number_float;
        }
        else if (t == value_t::string)
        {
            m_scratch = *m_object->m_value.string;
        }
        else if (t == value_t::binary)
        {
            m_scratch = *m_object->m_value.binary;
        }
        else
        {
            throw std::runtime_error("reflection_iterator: cannot get value");
        }
        return m_scratch;
    }

    pointer operator->() const
    {
        return &operator*();
    }

    reflection_iterator& operator++()
    {
        if (m_mode == 1)
        {
            std::advance(m_object_it, 1);
        }
        else if (m_mode == 2)
        {
            std::advance(m_array_it, 1);
        }
        else
        {
            ++m_primitive;
        }
        return *this;
    }
    reflection_iterator operator++(int)&
    {
        auto result = *this;
        ++(*this);
        return result;
    }
    reflection_iterator& operator--()
    {
        if (m_mode == 1)
        {
            std::advance(m_object_it, -1);
        }
        else if (m_mode == 2)
        {
            std::advance(m_array_it, -1);
        }
        else
        {
            --m_primitive;
        }
        return *this;
    }
    reflection_iterator operator--(int)&
    {
        auto result = *this;
        --(*this);
        return result;
    }

    // --- comparisons (same-object requirement, mirroring iter_impl) ---
    bool operator==(const reflection_iterator& other) const
    {
        if (m_object != other.m_object)
        {
            throw std::runtime_error("cannot compare iterators of different containers");
        }
        if (m_object == nullptr)
        {
            return true;
        }
        if (m_mode == 1)
        {
            return m_object_it == other.m_object_it;
        }
        if (m_mode == 2)
        {
            return m_array_it == other.m_array_it;
        }
        return m_primitive == other.m_primitive;
    }
    bool operator!=(const reflection_iterator& other) const
    {
        return !(*this == other);
    }
    bool operator<(const reflection_iterator& other) const
    {
        if (m_object != other.m_object)
        {
            throw std::runtime_error("cannot compare iterators of different containers");
        }
        if (m_object == nullptr)
        {
            return false;
        }
        if (m_mode == 1)
        {
            throw std::runtime_error("cannot compare order of object iterators");
        }
        if (m_mode == 2)
        {
            return m_array_it < other.m_array_it;
        }
        return m_primitive < other.m_primitive;
    }
    bool operator<=(const reflection_iterator& other) const
    {
        return !(other < *this);
    }
    bool operator>(const reflection_iterator& other) const
    {
        return !(*this <= other);
    }
    bool operator>=(const reflection_iterator& other) const
    {
        return !(*this < other);
    }

    reflection_iterator& operator+=(const difference_type i)
    {
        if (m_mode == 1)
        {
            throw std::runtime_error("cannot use offsets with object iterators");
        }
        if (m_mode == 2)
        {
            std::advance(m_array_it, i);
        }
        else
        {
            m_primitive += i;
        }
        return *this;
    }
    reflection_iterator& operator-=(const difference_type i)
    {
        return operator+=(-i);
    }
    reflection_iterator operator+(const difference_type i) const
    {
        auto result = *this;
        result += i;
        return result;
    }
    friend reflection_iterator operator+(const difference_type i, const reflection_iterator& it)
    {
        auto result = it;
        result += i;
        return result;
    }
    reflection_iterator operator-(const difference_type i) const
    {
        auto result = *this;
        result -= i;
        return result;
    }
    difference_type operator-(const reflection_iterator& other) const
    {
        if (m_mode == 1)
        {
            throw std::runtime_error("cannot use offsets with object iterators");
        }
        if (m_mode == 2)
        {
            return m_array_it - other.m_array_it;
        }
        return m_primitive - other.m_primitive;
    }

    reference operator[](const difference_type n) const
    {
        if (m_mode == 1)
        {
            throw std::runtime_error("cannot use operator[] for object iterators");
        }
        if (m_mode == 2)
        {
            return *std::next(m_array_it, n);
        }
        if (m_primitive == -n)
        {
            return operator*();
        }
        throw std::runtime_error("reflection_iterator: cannot get value");
    }

    // the key of an object iterator (iter_impl::key)
    const json::object_t::key_type& key() const
    {
        if (m_object->is_object())
        {
            return m_object_it->first;
        }
        throw std::runtime_error("cannot use key() for non-object iterators");
    }

    reference value() const
    {
        return operator*();
    }
};

// ---------------------------------------------------------------------------
// basic_json_reflection iterator members (out of line — they need the
// complete reflection_iterator defined above)
// ---------------------------------------------------------------------------
inline basic_json_reflection::iterator basic_json_reflection::begin() noexcept
{
    iterator it(this);
    it.set_begin();
    return it;
}
inline basic_json_reflection::iterator basic_json_reflection::end() noexcept
{
    iterator it(this);
    it.set_end();
    return it;
}
inline basic_json_reflection::const_iterator basic_json_reflection::begin() const noexcept
{
    const_iterator it(this);
    it.set_begin();
    return it;
}
inline basic_json_reflection::const_iterator basic_json_reflection::end() const noexcept
{
    const_iterator it(this);
    it.set_end();
    return it;
}
inline basic_json_reflection::const_iterator basic_json_reflection::cbegin() const noexcept
{
    return begin();
}
inline basic_json_reflection::const_iterator basic_json_reflection::cend() const noexcept
{
    return end();
}
inline basic_json_reflection::reverse_iterator basic_json_reflection::rbegin() noexcept
{
    return reverse_iterator(end());
}
inline basic_json_reflection::reverse_iterator basic_json_reflection::rend() noexcept
{
    return reverse_iterator(begin());
}
inline basic_json_reflection::const_reverse_iterator basic_json_reflection::rbegin() const noexcept
{
    return const_reverse_iterator(end());
}
inline basic_json_reflection::const_reverse_iterator basic_json_reflection::rend() const noexcept
{
    return const_reverse_iterator(begin());
}
inline basic_json_reflection::const_reverse_iterator basic_json_reflection::crbegin() const noexcept
{
    return rbegin();
}
inline basic_json_reflection::const_reverse_iterator basic_json_reflection::crend() const noexcept
{
    return rend();
}

inline json& basic_json_reflection::operator[](const std::size_t idx)
{
    if (is_null())
    {
        // implicitly convert a null value to an empty array (library semantics)
        m_type = value_t::array;
        construct_one<value_t::array>(m_value);
    }
    if (!is_array())
    {
        throw std::runtime_error(std::string("cannot use operator[] with a numeric argument with ") + type_name());
    }
    if (idx >= m_value.array->size())
    {
        // fill up the array with null values if given idx is outside the range
        m_value.array->resize(idx + 1);
    }
    return m_value.array->operator[](idx);
}

inline json& basic_json_reflection::operator[](const json::object_t::key_type& key)
{
    if (is_null())
    {
        // implicitly convert a null value to an empty object (library semantics)
        m_type = value_t::object;
        construct_one<value_t::object>(m_value);
    }
    if (!is_object())
    {
        throw std::runtime_error(std::string("cannot use operator[] with a string argument with ") + type_name());
    }
    auto result = m_value.object->emplace(key, nullptr);
    return result.first->second;
}

inline basic_json_reflection::iterator basic_json_reflection::find(const json::object_t::key_type& key) noexcept
{
    if (is_object())
    {
        iterator it(this);
        it.m_object_it = m_value.object->find(key);
        return it;
    }
    return end();
}

inline bool basic_json_reflection::contains(const json::object_t::key_type& key) const noexcept
{
    return is_object() && m_value.object->find(key) != m_value.object->end();
}

inline std::size_t basic_json_reflection::count(const json::object_t::key_type& key) const noexcept
{
    return is_object() ? m_value.object->count(key) : 0;
}

inline basic_json_reflection::iterator basic_json_reflection::erase(iterator pos)
{
    if (this != pos.m_object)
    {
        throw std::runtime_error("iterators are not compatible");
    }
    iterator result = end();
    template for (constexpr auto r : kValueTInfos)
    {
        constexpr value_t V = static_cast<value_t>([: r :]);
        if (m_type == V)
        {
            if constexpr (V == value_t::object)
            {
                result.m_object_it = m_value.object->erase(pos.m_object_it);
            }
            else if constexpr (V == value_t::array)
            {
                result.m_array_it = m_value.array->erase(pos.m_array_it);
            }
            else if constexpr (V == value_t::null || V == value_t::discarded)
            {
                throw std::runtime_error(std::string("cannot use erase() with ") + type_name());
            }
            else
            {
                // scalar/string/binary: erasing the single element resets the
                // value to null (string/binary pointers freed by destroy())
                destroy();
            }
        }
    }
    return result;
}

inline basic_json_reflection::iterator basic_json_reflection::erase(iterator first, iterator last)
{
    if (this != first.m_object || this != last.m_object)
    {
        throw std::runtime_error("iterators are not compatible");
    }
    iterator result = end();
    template for (constexpr auto r : kValueTInfos)
    {
        constexpr value_t V = static_cast<value_t>([: r :]);
        if (m_type == V)
        {
            if constexpr (V == value_t::object)
            {
                result.m_object_it = m_value.object->erase(first.m_object_it, last.m_object_it);
            }
            else if constexpr (V == value_t::array)
            {
                result.m_array_it = m_value.array->erase(first.m_array_it, last.m_array_it);
            }
            else if constexpr (V == value_t::null || V == value_t::discarded)
            {
                throw std::runtime_error(std::string("cannot use erase() with ") + type_name());
            }
            else
            {
                if (first.m_primitive != 0 || last.m_primitive != 1)
                {
                    throw std::runtime_error("iterators out of range");
                }
                destroy();
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Reflection-driven serializer.
//
// The value dispatch is NOT a hand-written switch over value_t: it is a
// `template for` over the reflection-gathered enumerator set (kValueTInfos)
// with a per-enumerator NTTP action (dump_one<V>). Adding a value_t without
// wiring it here is a compile error, not a silent leak. The only place a
// literal value_t name appears is inside dump_one<V>'s if constexpr chain,
// which is the "format of that type" concern — the ROUTING table is the
// reflection enumeration itself.
// ---------------------------------------------------------------------------
struct reflection_serializer
{
    std::string out;

    explicit reflection_serializer(const basic_json_reflection& j, const bool ensure_ascii_ = false)
        : ensure_ascii(ensure_ascii_)
    {
        dump(j);
    }

    std::string str() const
    {
        return out;
    }

  private:
    bool ensure_ascii;
    std::string buffer; // scratch for escaped strings

    void dump(const basic_json_reflection& j)
    {
        const value_t t = j.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                dump_one<V>(j);
            }
        }
    }

    // --- per-type dump actions (reflection-dispatched) ---
    template<value_t V>
    void dump_one(const basic_json_reflection& j)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::null)
        {
            out += "null";
        }
        else if constexpr (V == value_t::object)
        {
            dump_object(*u.object);
        }
        else if constexpr (V == value_t::array)
        {
            dump_array(*u.array);
        }
        else if constexpr (V == value_t::string)
        {
            out.push_back('"');
            dump_escaped(*u.string, ensure_ascii);
            out.push_back('"');
        }
        else if constexpr (V == value_t::binary)
        {
            dump_binary(*u.binary);
        }
        else if constexpr (V == value_t::boolean)
        {
            out += u.boolean ? "true" : "false";
        }
        else if constexpr (V == value_t::number_integer)
        {
            dump_number(u.number_integer);
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            dump_number(u.number_unsigned);
        }
        else if constexpr (V == value_t::number_float)
        {
            dump_float(u.number_float);
        }
        else if constexpr (V == value_t::discarded)
        {
            out += "<discarded>";
        }
    }

    void dump_object(const json::object_t& obj)
    {
        out.push_back('{');
        std::size_t idx = 0;
        for (auto it = obj.cbegin(); it != obj.cend(); ++it, ++idx)
        {
            if (idx)
            {
                out.push_back(',');
            }
            out.push_back('"');
            dump_escaped(it->first, ensure_ascii);
            out.push_back('"');
            out.push_back(':');
            basic_json_reflection val;
            val.assign_from(it->second);
            dump(val);
        }
        out.push_back('}');
    }

    void dump_array(const json::array_t& arr)
    {
        out.push_back('[');
        std::size_t idx = 0;
        for (const auto& el : arr)
        {
            if (idx++)
            {
                out.push_back(',');
            }
            basic_json_reflection val;
            val.assign_from(el);
            dump(val);
        }
        out.push_back(']');
    }

    void dump_binary(const json::binary_t& bin)
    {
        out += "{\"bytes\":[";
        for (std::size_t i = 0; i < bin.size(); ++i)
        {
            if (i)
            {
                out.push_back(',');
            }
            dump_number(bin[i]);
        }
        out += "],\"subtype\":";
        if (bin.has_subtype())
        {
            dump_number(bin.subtype());
            out.push_back('}');
        }
        else
        {
            out += "null}";
        }
    }

    template<typename T>
    void dump_number(const T n)
    {
        char tmp[32];
        auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), n);
        out.append(tmp, p);
    }

    void dump_float(const double f)
    {
        // match the library's default dump for integral floats / general
        char tmp[48];
        if (!std::isfinite(f))
        {
            // the library dumps NaN AND +/-inf as "null" (dump_float,
            // serializer.hpp: `if (!std::isfinite(x)) { write "null"; }`)
            out += "null";
            return;
        }
        if (std::signbit(f) && f == 0.0)
        {
            out += "-0.0"; // the library dumps negative zero as "-0.0"
            return;
        }
        if (f == static_cast<long long>(f) && std::abs(f) < 1e17)
        {
            // integral value -> "N.N0" like the library (e.g. 3.0 -> "3.0")
            auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), static_cast<long long>(f));
            out.append(tmp, p);
            out += ".0";
            return;
        }
        // general shortest round-trip
        auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), f);
        out.append(tmp, p);
    }

    // escape per the library: control chars, quotes, backslash; ensure_ascii
    // additionally escapes non-ASCII
    void dump_escaped(const std::string& s, const bool ascii)
    {
        buffer.clear();
        for (unsigned char c : s)
        {
            switch (c)
            {
                case 0x22:
                    buffer += "\\\"";
                    break;
                case 0x5C:
                    buffer += "\\\\";
                    break;
                case 0x08:
                    buffer += "\\b";
                    break;
                case 0x09:
                    buffer += "\\t";
                    break;
                case 0x0A:
                    buffer += "\\n";
                    break;
                case 0x0C:
                    buffer += "\\f";
                    break;
                case 0x0D:
                    buffer += "\\r";
                    break;
                default:
                    if (c < 0x20)
                    {
                        char hex[7];
                        std::snprintf(hex, sizeof hex, "\\u%04x", c);
                        buffer += hex;
                    }
                    else if (ascii && c >= 0x7F)
                    {
                        // escape the leading byte of a UTF-8 seq as \u00XX
                        // (adequate for the ASCII range differential cases)
                        char hex[7];
                        std::snprintf(hex, sizeof hex, "\\u00%02x", c);
                        buffer += hex;
                    }
                    else
                    {
                        buffer.push_back(static_cast<char>(c));
                    }
            }
        }
        out += buffer;
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven CBOR writer.
//
// Same dispatch principle as the JSON serializer: value routing comes from the
// reflection enumerator set (kValueTInfos) via `template for` + per-enumerator
// NTTP actions (cbor_one<V>). Byte-level encoding replicates nlohmann's CBOR
// writer (integer/main-type width selection, compact float prefix, container
// length headers) so the output is byte-identical (differential-tested).
// ---------------------------------------------------------------------------
struct reflection_cbor_serializer
{
    std::vector<std::uint8_t> out;

    explicit reflection_cbor_serializer(const basic_json_reflection& j)
    {
        dump(j);
    }

    std::string str() const
    {
        return std::string(out.begin(), out.end());
    }
    const std::vector<std::uint8_t>& bytes() const
    {
        return out;
    }

  private:
    static bool little_endian()
    {
        const uint16_t x = 1;
        return *reinterpret_cast<const uint8_t*>(&x) == 1;
    }

    // append value as big-endian bytes
    template<typename T>
    void append_big(T v)
    {
        const auto n = static_cast<std::size_t>(sizeof(T));
        std::array<uint8_t, sizeof(T)> tmp{};
        std::memcpy(tmp.data(), &v, n);
        if (little_endian())
        {
            std::reverse(tmp.begin(), tmp.end());
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    }

    // CBOR length prefix for a given major type (0..7) and unsigned length
    void prefix(std::uint8_t major, std::uint64_t len)
    {
        const auto mt = static_cast<std::uint8_t>(major << 5);
        if (len <= 23)
        {
            out.push_back(mt + static_cast<std::uint8_t>(len));
        }
        else if (len <= 0xFF)
        {
            out.push_back(mt + 24);
            out.push_back(static_cast<std::uint8_t>(len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(mt + 25);
            append_big(static_cast<std::uint16_t>(len));
        }
        else if (len <= 0xFFFFFFFFu)
        {
            out.push_back(mt + 26);
            append_big(static_cast<std::uint32_t>(len));
        }
        else
        {
            out.push_back(mt + 27);
            append_big(static_cast<std::uint64_t>(len));
        }
    }

    // CBOR negative integer: encode -(len+1) with major 1
    void prefix_negative(std::int64_t n)
    {
        const std::uint64_t len = static_cast<std::uint64_t>(-1 - n);
        prefix(1, len);
    }

    void dump(const basic_json_reflection& j)
    {
        const value_t t = j.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                cbor_one<V>(j);
            }
        }
    }

    template<value_t V>
    void cbor_one(const basic_json_reflection& j)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::null)
        {
            out.push_back(0xF6);
        }
        else if constexpr (V == value_t::object)
        {
            cbor_object(*u.object);
        }
        else if constexpr (V == value_t::array)
        {
            cbor_array(*u.array);
        }
        else if constexpr (V == value_t::string)
        {
            const auto& s = *u.string;
            prefix(3, s.size());
            out.insert(out.end(), s.begin(), s.end());
        }
        else if constexpr (V == value_t::binary)
        {
            cbor_binary(*u.binary);
        }
        else if constexpr (V == value_t::boolean)
        {
            out.push_back(u.boolean ? 0xF5 : 0xF4);
        }
        else if constexpr (V == value_t::number_integer)
        {
            const auto n = u.number_integer;
            if (n >= 0)
            {
                prefix(0, static_cast<std::uint64_t>(n));
            }
            else
            {
                prefix_negative(n);
            }
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            prefix(0, u.number_unsigned);
        }
        else if constexpr (V == value_t::number_float)
        {
            cbor_float(u.number_float);
        }
        else if constexpr (V == value_t::discarded)
        {
            // the library writes nothing for discarded in CBOR
        }
    }

    void cbor_object(const json::object_t& obj)
    {
        prefix(5, obj.size());
        for (const auto& [k, v] : obj)
        {
            prefix(3, k.size());
            out.insert(out.end(), k.begin(), k.end());
            basic_json_reflection val;
            val.assign_from(v);
            dump(val);
        }
    }

    void cbor_array(const json::array_t& arr)
    {
        prefix(4, arr.size());
        for (const auto& el : arr)
        {
            basic_json_reflection val;
            val.assign_from(el);
            dump(val);
        }
    }

    void cbor_binary(const json::binary_t& bin)
    {
        // subtype tag (major 6) comes BEFORE the byte string, when present
        if (bin.has_subtype())
        {
            const auto st = bin.subtype();
            if (st <= 0xFF)
            {
                out.push_back(0xD8);
                out.push_back(static_cast<std::uint8_t>(st));
            }
            else if (st <= 0xFFFF)
            {
                out.push_back(0xD9);
                append_big(static_cast<std::uint16_t>(st));
            }
            else if (st <= 0xFFFFFFFFu)
            {
                out.push_back(0xDA);
                append_big(static_cast<std::uint32_t>(st));
            }
            else
            {
                out.push_back(0xDB);
                append_big(static_cast<std::uint64_t>(st));
            }
        }
        // byte string body: major type 2
        prefix(2, bin.size());
        out.insert(out.end(), bin.begin(), bin.end());
    }

    void cbor_float(const double n)
    {
        // replicate write_compact_float for CBOR (cbor)
        constexpr double F_MIN = static_cast<double>((std::numeric_limits<float>::lowest)());
        constexpr double F_MAX = static_cast<double>((std::numeric_limits<float>::max)());
        const bool use_float =
            !std::isfinite(n) ||
            (n >= F_MIN && n <= F_MAX &&
             static_cast<double>(static_cast<float>(n)) == n);
        if (use_float)
        {
            out.push_back(0xFA);
            append_big(static_cast<float>(n));
        }
        else
        {
            out.push_back(0xFB);
            append_big(n);
        }
    }
};

// ---------------------------------------------------------------------------
// value_t -> binary-format byte-code tables (reflection-generated).
//
// Each table is indexed by the union SLOT index (0..7, the same indexing as
// kStorage/kMemberIds: object, array, string, binary, boolean, number_integer,
// number_unsigned, number_float) and holds the format's "primary" byte code
// for that value type. 0xFF marks a value whose code is RANGE-DEPENDENT and
// is therefore selected at runtime inside the writer action (e.g. MessagePack
// fixnum/fixstr/fixarray vs the 8/16/32 forms, UBJSON int8..int64 narrowing,
// BSON int32/int64 narrowing) — the writer's per-enumerator action handles
// those. The tables are generated with the same consteval machinery as
// kStorage/kMemberIds: the value_t -> slot mapping (slot_index) is the single
// source of truth, so a value_t added without a slot_index specialization is
// a compile error.
// ---------------------------------------------------------------------------
namespace refl_detail
{
// slot order matches the real json_value member list (see kMemberIds)
constexpr std::array<value_t, 8> kSlotValueT =
{
    value_t::object, value_t::array, value_t::string, value_t::binary,
    value_t::boolean, value_t::number_integer, value_t::number_unsigned,
    value_t::number_float
};

// --- MessagePack: primary codes (0xFF = range-dependent) -------------------
template<value_t V> struct msgpack_code
{
    static constexpr std::uint8_t value = 0xFF;
};
template<> struct msgpack_code<value_t::null> { static constexpr std::uint8_t value = 0xC0; }; // nil
template<> struct msgpack_code<value_t::boolean> { static constexpr std::uint8_t value = 0xC3; }; // true; false is 0xC2
template<> struct msgpack_code<value_t::string> { static constexpr std::uint8_t value = 0xD9; }; // str8; fixstr/str16/str32 are range-dependent
template<> struct msgpack_code<value_t::array> { static constexpr std::uint8_t value = 0xDC; }; // array16; fixarray/array32 range-dependent
template<> struct msgpack_code<value_t::object> { static constexpr std::uint8_t value = 0xDE; }; // map16; fixmap/map32 range-dependent
template<> struct msgpack_code<value_t::binary> { static constexpr std::uint8_t value = 0xC4; }; // bin8; bin16/32 and ext/fixext variants
template<> struct msgpack_code<value_t::number_float> { static constexpr std::uint8_t value = 0xCA; }; // float32; float64 is 0xCB
// number_integer / number_unsigned: fully range-dependent (fixnum + uint/int 8..64)

// --- UBJSON: primary codes ------------------------------------------------
template<value_t V> struct ubjson_code
{
    static constexpr std::uint8_t value = 0xFF;
};
template<> struct ubjson_code<value_t::null> { static constexpr std::uint8_t value = 'Z'; };
template<> struct ubjson_code<value_t::boolean> { static constexpr std::uint8_t value = 'T'; }; // false is 'F'
template<> struct ubjson_code<value_t::string> { static constexpr std::uint8_t value = 'S'; };
template<> struct ubjson_code<value_t::array> { static constexpr std::uint8_t value = '['; };
template<> struct ubjson_code<value_t::object> { static constexpr std::uint8_t value = '{'; };
template<> struct ubjson_code<value_t::binary> { static constexpr std::uint8_t value = '['; };
template<> struct ubjson_code<value_t::number_float> { static constexpr std::uint8_t value = 'd'; }; // float32; float64 is 'D'
// number_integer / number_unsigned: range-dependent ('i'/'U'/'I'/'l'/'L', 'H' high-precision)

// --- BSON: element-type codes (all fixed except integer narrowing) --------
template<value_t V> struct bson_code
{
    static constexpr std::uint8_t value = 0xFF;
};
template<> struct bson_code<value_t::object> { static constexpr std::uint8_t value = 0x03; };
template<> struct bson_code<value_t::array> { static constexpr std::uint8_t value = 0x04; };
template<> struct bson_code<value_t::string> { static constexpr std::uint8_t value = 0x02; };
template<> struct bson_code<value_t::binary> { static constexpr std::uint8_t value = 0x05; };
template<> struct bson_code<value_t::boolean> { static constexpr std::uint8_t value = 0x08; };
template<> struct bson_code<value_t::null> { static constexpr std::uint8_t value = 0x0A; };
template<> struct bson_code<value_t::number_integer> { static constexpr std::uint8_t value = 0x10; }; // int32; int64 is 0x12
template<> struct bson_code<value_t::number_unsigned> { static constexpr std::uint8_t value = 0x10; }; // int32; int64/uint64 are 0x12/0x11
template<> struct bson_code<value_t::number_float> { static constexpr std::uint8_t value = 0x01; };

template<template<value_t> class CodeOf, std::size_t... I>
consteval auto codes_impl(std::index_sequence<I...>)
{
    return std::array<std::uint8_t, sizeof...(I)> { CodeOf<kSlotValueT[I]>::value... };
}

// --- BSON LOAD table (read direction): byte code -> {union slot, payload kind}
// The writer table (bson_code, above) maps value_t -> primary byte code; the
// reader needs one more dimension — 0x10 (int32) and 0x12 (int64) map to the
// SAME value_t (number_integer) but carry different payload widths — so the
// reverse table entries carry a payload kind, not just a value_t. Generated
// from 9 consteval specializations (one per BSON element type), indexed by
// the raw byte code; unused codes are invalid.
enum class bson_payload_kind : std::uint8_t
{
    double_fixed,   // 8-byte LE double
    string_len,     // int32 len + bytes + 0x00
    object_doc,     // nested document -> object
    array_doc,      // nested document -> array
    binary_len,     // int32 len + 1-byte subtype + bytes
    boolean_byte,   // 1 byte
    null_fixed,     // no payload
    int32_le,       // 4-byte LE signed
    int64_le,       // 8-byte LE signed
    uint64_le,      // 8-byte LE unsigned
    invalid = 0xFF  // must come LAST — enumerators increment from here
};

struct bson_load_entry
{
    std::uint8_t slot;  // union slot index 0..7, or 0xFF when the type has no slot (null)
    std::uint8_t kind;  // bson_payload_kind
};

template<std::uint8_t Code> struct bson_load_code
{
    static constexpr bson_load_entry value = {0xFF, static_cast<std::uint8_t>(bson_payload_kind::invalid)};
};
template<> struct bson_load_code<0x01> { static constexpr bson_load_entry value = {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(bson_payload_kind::double_fixed)}; };
template<> struct bson_load_code<0x02> { static constexpr bson_load_entry value = {slot_index<value_t::string>::value, static_cast<std::uint8_t>(bson_payload_kind::string_len)}; };
template<> struct bson_load_code<0x03> { static constexpr bson_load_entry value = {slot_index<value_t::object>::value, static_cast<std::uint8_t>(bson_payload_kind::object_doc)}; };
template<> struct bson_load_code<0x04> { static constexpr bson_load_entry value = {slot_index<value_t::array>::value, static_cast<std::uint8_t>(bson_payload_kind::array_doc)}; };
template<> struct bson_load_code<0x05> { static constexpr bson_load_entry value = {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(bson_payload_kind::binary_len)}; };
template<> struct bson_load_code<0x08> { static constexpr bson_load_entry value = {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(bson_payload_kind::boolean_byte)}; };
template<> struct bson_load_code<0x0A> { static constexpr bson_load_entry value = {0xFF, static_cast<std::uint8_t>(bson_payload_kind::null_fixed)}; }; // null: no union slot
template<> struct bson_load_code<0x10> { static constexpr bson_load_entry value = {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(bson_payload_kind::int32_le)}; };
template<> struct bson_load_code<0x12> { static constexpr bson_load_entry value = {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(bson_payload_kind::int64_le)}; };
template<> struct bson_load_code<0x11> { static constexpr bson_load_entry value = {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(bson_payload_kind::uint64_le)}; };

template<std::size_t... I>
consteval auto load_table_impl(std::index_sequence<I...>)
{
    return std::array<bson_load_entry, sizeof...(I)>
    {
        bson_load_code<static_cast<std::uint8_t>(I)>::value...
    };
}
} // namespace refl_detail

// primary-code tables, one per binary format (indexed by union slot, 0..7)
constexpr auto kMsgpackCodes =
    refl_detail::codes_impl<refl_detail::msgpack_code>(std::make_index_sequence<8> {});
constexpr auto kUbjsonCodes =
    refl_detail::codes_impl<refl_detail::ubjson_code>(std::make_index_sequence<8> {});
constexpr auto kBsonCodes =
    refl_detail::codes_impl<refl_detail::bson_code>(std::make_index_sequence<8> {});

// reverse BSON table: raw element-type byte -> load entry (read direction)
constexpr auto kBsonsLoad =
    refl_detail::load_table_impl(std::make_index_sequence<256> {});

// the tables cover exactly the 8 union slots; null/discarded have no slot
static_assert(kMsgpackCodes.size() == 8 && kUbjsonCodes.size() == 8 && kBsonCodes.size() == 8,
              "binary byte-code tables must cover every json_value member");
static_assert(kUbjsonCodes[slot_index<value_t::null>::has ? 0 : 0] == kUbjsonCodes[0], "");

// BSON load-table completeness: each of the 9 element types has a legal entry,
// and the union slots match the writer table / the value_t enumeration —
// adding a value_t or changing a BSON element type is a compile error, never
// a silent parse drift.
static_assert(kBsonsLoad.size() == 256, "BSON load table must cover the whole byte range");
static_assert(kBsonsLoad[0x01].slot == slot_index<value_t::number_float>::value &&
              kBsonsLoad[0x01].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::double_fixed) &&
              kBsonCodes[slot_index<value_t::number_float>::value] == 0x01, "BSON double entry");
static_assert(kBsonsLoad[0x02].slot == slot_index<value_t::string>::value &&
              kBsonsLoad[0x02].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::string_len) &&
              kBsonCodes[slot_index<value_t::string>::value] == 0x02, "BSON string entry");
static_assert(kBsonsLoad[0x03].slot == slot_index<value_t::object>::value &&
              kBsonsLoad[0x03].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::object_doc) &&
              kBsonCodes[slot_index<value_t::object>::value] == 0x03, "BSON object entry");
static_assert(kBsonsLoad[0x04].slot == slot_index<value_t::array>::value &&
              kBsonsLoad[0x04].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::array_doc) &&
              kBsonCodes[slot_index<value_t::array>::value] == 0x04, "BSON array entry");
static_assert(kBsonsLoad[0x05].slot == slot_index<value_t::binary>::value &&
              kBsonsLoad[0x05].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::binary_len) &&
              kBsonCodes[slot_index<value_t::binary>::value] == 0x05, "BSON binary entry");
static_assert(kBsonsLoad[0x08].slot == slot_index<value_t::boolean>::value &&
              kBsonsLoad[0x08].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::boolean_byte) &&
              kBsonCodes[slot_index<value_t::boolean>::value] == 0x08, "BSON boolean entry");
static_assert(kBsonsLoad[0x0A].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::null_fixed),
              "BSON null entry"); // null has no union slot, so no slot check
static_assert(kBsonsLoad[0x10].slot == slot_index<value_t::number_integer>::value &&
              kBsonsLoad[0x10].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::int32_le), "BSON int32 entry");
static_assert(kBsonsLoad[0x12].slot == slot_index<value_t::number_integer>::value &&
              kBsonsLoad[0x12].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::int64_le), "BSON int64 entry");
static_assert(kBsonsLoad[0x11].slot == slot_index<value_t::number_unsigned>::value &&
              kBsonsLoad[0x11].kind == static_cast<std::uint8_t>(refl_detail::bson_payload_kind::uint64_le), "BSON uint64 entry");

// ---------------------------------------------------------------------------
// CBOR / MSGPACK / UBJSON LOAD tables (read directions, M4B-3).
//
// Mirror of kBsonsLoad: a 256-entry reverse table mapping every possible
// first byte of a value to {union slot, payload kind}. Unlike BSON (9 fixed
// element types), CBOR/MSGPACK/UBJSON fill most of the byte space with
// range-dependent forms (fixints, fixstr/fixmap/fixarray, indefinite
// lengths, CBOR tags), so the tables are built by a consteval range
// classifier (cbor_entry_for / msgpack_entry_for / ubjson_entry_for) instead
// of one specialization per byte — the union slots still come from the
// reflection-generated slot_index<value_t>, so a value_t added without a
// slot_index specialization is still a compile error. Slot 0xFF marks a byte
// whose value type has no union storage (null, tags, '$'/'#' markers).
// ---------------------------------------------------------------------------
namespace refl_detail
{
enum class cbor_payload_kind : std::uint8_t
{
    uint_immediate,   // 0x00-0x17: value = code
    uint8, uint16, uint32, uint64,          // 0x18-0x1B
    negint_immediate, // 0x20-0x37: value = -1 - (code & 0x1F)
    negint8, negint16, negint32, negint64,  // 0x38-0x3B
    bytes_immediate,  // 0x40-0x57: len = code & 0x1F
    bytes8, bytes16, bytes32, bytes64,      // 0x58-0x5B
    bytes_indefinite, // 0x5F
    text_immediate,   // 0x60-0x77
    text8, text16, text32, text64,          // 0x78-0x7B
    text_indefinite,  // 0x7F
    array_immediate,  // 0x80-0x97
    array8, array16, array32, array64,      // 0x98-0x9B
    array_indefinite, // 0x9F
    map_immediate,    // 0xA0-0xB7
    map8, map16, map32, map64,              // 0xB8-0xBB
    map_indefinite,   // 0xBF
    tag_noarg,        // 0xC6-0xD7 (tag value implicit, no payload)
    tag_1byte,        // 0xD8
    tag_2byte,        // 0xD9
    tag_4byte,        // 0xDA
    tag_8byte,        // 0xDB
    boolean_false,    // 0xF4
    boolean_true,     // 0xF5
    null_fixed,       // 0xF6
    half_float,       // 0xF9
    float32,          // 0xFA
    float64,          // 0xFB
    invalid = 0xFF
};

struct cbor_load_entry
{
    std::uint8_t slot;  // union slot index 0..7, or 0xFF for no-slot bytes
    std::uint8_t kind;  // cbor_payload_kind
};

consteval cbor_load_entry cbor_entry_for(const std::uint8_t b)
{
    // major 0 (unsigned integer)
    if (b <= 0x17)
    {
        return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(cbor_payload_kind::uint_immediate)};
    }
    if (b <= 0x1B)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::uint8),
            static_cast<std::uint8_t>(cbor_payload_kind::uint16),
            static_cast<std::uint8_t>(cbor_payload_kind::uint32),
            static_cast<std::uint8_t>(cbor_payload_kind::uint64)
        };
        return {slot_index<value_t::number_unsigned>::value, kinds[b - 0x18]};
    }
    if (b <= 0x1F)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 1 (negative integer)
    if (b <= 0x37)
    {
        return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(cbor_payload_kind::negint_immediate)};
    }
    if (b <= 0x3B)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::negint8),
            static_cast<std::uint8_t>(cbor_payload_kind::negint16),
            static_cast<std::uint8_t>(cbor_payload_kind::negint32),
            static_cast<std::uint8_t>(cbor_payload_kind::negint64)
        };
        return {slot_index<value_t::number_integer>::value, kinds[b - 0x38]};
    }
    if (b <= 0x3F)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 2 (byte string)
    if (b <= 0x57)
    {
        return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(cbor_payload_kind::bytes_immediate)};
    }
    if (b <= 0x5B)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::bytes8),
            static_cast<std::uint8_t>(cbor_payload_kind::bytes16),
            static_cast<std::uint8_t>(cbor_payload_kind::bytes32),
            static_cast<std::uint8_t>(cbor_payload_kind::bytes64)
        };
        return {slot_index<value_t::binary>::value, kinds[b - 0x58]};
    }
    if (b == 0x5F)
    {
        return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(cbor_payload_kind::bytes_indefinite)};
    }
    if (b <= 0x5E)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 3 (text string)
    if (b <= 0x77)
    {
        return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(cbor_payload_kind::text_immediate)};
    }
    if (b <= 0x7B)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::text8),
            static_cast<std::uint8_t>(cbor_payload_kind::text16),
            static_cast<std::uint8_t>(cbor_payload_kind::text32),
            static_cast<std::uint8_t>(cbor_payload_kind::text64)
        };
        return {slot_index<value_t::string>::value, kinds[b - 0x78]};
    }
    if (b == 0x7F)
    {
        return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(cbor_payload_kind::text_indefinite)};
    }
    if (b <= 0x7E)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 4 (array)
    if (b <= 0x97)
    {
        return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(cbor_payload_kind::array_immediate)};
    }
    if (b <= 0x9B)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::array8),
            static_cast<std::uint8_t>(cbor_payload_kind::array16),
            static_cast<std::uint8_t>(cbor_payload_kind::array32),
            static_cast<std::uint8_t>(cbor_payload_kind::array64)
        };
        return {slot_index<value_t::array>::value, kinds[b - 0x98]};
    }
    if (b == 0x9F)
    {
        return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(cbor_payload_kind::array_indefinite)};
    }
    if (b <= 0x9E)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 5 (map)
    if (b <= 0xB7)
    {
        return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(cbor_payload_kind::map_immediate)};
    }
    if (b <= 0xBB)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::map8),
            static_cast<std::uint8_t>(cbor_payload_kind::map16),
            static_cast<std::uint8_t>(cbor_payload_kind::map32),
            static_cast<std::uint8_t>(cbor_payload_kind::map64)
        };
        return {slot_index<value_t::object>::value, kinds[b - 0xB8]};
    }
    if (b == 0xBF)
    {
        return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(cbor_payload_kind::map_indefinite)};
    }
    if (b <= 0xBE)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // major 6 (tag): 0xC0-0xC5 and 0xDC-0xDF are rejected like the library
    // (which only handles 0xC6-0xDB); 0xD8-0xDB carry a 1/2/4/8-byte tag value
    if (b <= 0xC5)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)};
    }
    if (b <= 0xD7)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::tag_noarg)};
    }
    if (b <= 0xDB)
    {
        constexpr std::uint8_t kinds[] =
        {
            static_cast<std::uint8_t>(cbor_payload_kind::tag_1byte),
            static_cast<std::uint8_t>(cbor_payload_kind::tag_2byte),
            static_cast<std::uint8_t>(cbor_payload_kind::tag_4byte),
            static_cast<std::uint8_t>(cbor_payload_kind::tag_8byte)
        };
        return {0xFF, kinds[b - 0xD8]};
    }
    if (b <= 0xDF)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // reserved
    }
    // simple values / booleans / null / floats
    if (b <= 0xF3)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // simple values rejected like the library
    }
    if (b == 0xF4)
    {
        return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(cbor_payload_kind::boolean_false)};
    }
    if (b == 0xF5)
    {
        return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(cbor_payload_kind::boolean_true)};
    }
    if (b == 0xF6)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::null_fixed)};
    }
    if (b <= 0xF8)
    {
        return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // undefined / simple-with-byte rejected
    }
    if (b == 0xF9)
    {
        return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(cbor_payload_kind::half_float)};
    }
    if (b == 0xFA)
    {
        return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(cbor_payload_kind::float32)};
    }
    if (b == 0xFB)
    {
        return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(cbor_payload_kind::float64)};
    }
    return {0xFF, static_cast<std::uint8_t>(cbor_payload_kind::invalid)}; // 0xFC-0xFF (break is handled by the container loops)
}

enum class msgpack_payload_kind : std::uint8_t
{
    positive_fixint,  // 0x00-0x7F: value = code
    fixmap,           // 0x80-0x8F: count = code & 0x0F
    fixarray,         // 0x90-0x9F: count = code & 0x0F
    fixstr,           // 0xA0-0xBF: len = code & 0x1F
    nil_fixed,        // 0xC0
    boolean_false,    // 0xC2
    boolean_true,     // 0xC3
    bin8, bin16, bin32,       // 0xC4-0xC6
    ext8, ext16, ext32,       // 0xC7-0xC9
    float32, float64,         // 0xCA-0xCB
    uint8, uint16, uint32, uint64, // 0xCC-0xCF
    int8, int16, int32, int64,     // 0xD0-0xD3
    fixext1, fixext2, fixext4, fixext8, fixext16, // 0xD4-0xD8
    str8, str16, str32,       // 0xD9-0xDB
    array16, array32,         // 0xDC-0xDD
    map16, map32,             // 0xDE-0xDF
    negative_fixint,  // 0xE0-0xFF: value = (int8)code
    invalid = 0xFF    // 0xC1 never-used
};

struct msgpack_load_entry
{
    std::uint8_t slot;
    std::uint8_t kind;
};

consteval msgpack_load_entry msgpack_entry_for(const std::uint8_t b)
{
    if (b <= 0x7F)
    {
        return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(msgpack_payload_kind::positive_fixint)};
    }
    if (b <= 0x8F)
    {
        return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixmap)};
    }
    if (b <= 0x9F)
    {
        return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixarray)};
    }
    if (b <= 0xBF)
    {
        return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixstr)};
    }
    switch (b)
    {
        case 0xC0: return {0xFF, static_cast<std::uint8_t>(msgpack_payload_kind::nil_fixed)};
        case 0xC1: return {0xFF, static_cast<std::uint8_t>(msgpack_payload_kind::invalid)};
        case 0xC2: return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(msgpack_payload_kind::boolean_false)};
        case 0xC3: return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(msgpack_payload_kind::boolean_true)};
        case 0xC4: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::bin8)};
        case 0xC5: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::bin16)};
        case 0xC6: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::bin32)};
        case 0xC7: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::ext8)};
        case 0xC8: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::ext16)};
        case 0xC9: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::ext32)};
        case 0xCA: return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(msgpack_payload_kind::float32)};
        case 0xCB: return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(msgpack_payload_kind::float64)};
        case 0xCC: return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(msgpack_payload_kind::uint8)};
        case 0xCD: return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(msgpack_payload_kind::uint16)};
        case 0xCE: return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(msgpack_payload_kind::uint32)};
        case 0xCF: return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(msgpack_payload_kind::uint64)};
        case 0xD0: return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(msgpack_payload_kind::int8)};
        case 0xD1: return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(msgpack_payload_kind::int16)};
        case 0xD2: return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(msgpack_payload_kind::int32)};
        case 0xD3: return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(msgpack_payload_kind::int64)};
        case 0xD4: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixext1)};
        case 0xD5: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixext2)};
        case 0xD6: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixext4)};
        case 0xD7: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixext8)};
        case 0xD8: return {slot_index<value_t::binary>::value, static_cast<std::uint8_t>(msgpack_payload_kind::fixext16)};
        case 0xD9: return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(msgpack_payload_kind::str8)};
        case 0xDA: return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(msgpack_payload_kind::str16)};
        case 0xDB: return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(msgpack_payload_kind::str32)};
        case 0xDC: return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(msgpack_payload_kind::array16)};
        case 0xDD: return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(msgpack_payload_kind::array32)};
        case 0xDE: return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(msgpack_payload_kind::map16)};
        case 0xDF: return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(msgpack_payload_kind::map32)};
        default:
            break;
    }
    if (b >= 0xE0)
    {
        return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(msgpack_payload_kind::negative_fixint)};
    }
    return {0xFF, static_cast<std::uint8_t>(msgpack_payload_kind::invalid)};
}

enum class ubjson_payload_kind : std::uint8_t
{
    boolean_true,    // 'T'
    boolean_false,   // 'F'
    null_fixed,      // 'Z'
    noop,            // 'N'
    uint8,           // 'U'
    int8,            // 'i'
    int16,           // 'I'
    int32,           // 'l'
    int64,           // 'L'
    uint16_bjd,      // 'u' (bjdata only)
    uint32_bjd,      // 'm' (bjdata only)
    uint64_bjd,      // 'M' (bjdata only)
    half_float,      // 'h' (bjdata only)
    float32,         // 'd'
    float64,         // 'D'
    char_,           // 'C'
    string_,         // 'S'
    high_precision,  // 'H'
    array_,          // '['
    object_,         // '{'
    type_marker,     // '$'
    count_marker,    // '#'
    byte_bjd,        // 'B' (bjdata value marker -> uint8; optimized-container type -> binary)
    invalid = 0xFF
};

struct ubjson_load_entry
{
    std::uint8_t slot;
    std::uint8_t kind;
};

consteval ubjson_load_entry ubjson_entry_for(const std::uint8_t b)
{
    switch (b)
    {
        case 'T': return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(ubjson_payload_kind::boolean_true)};
        case 'F': return {slot_index<value_t::boolean>::value, static_cast<std::uint8_t>(ubjson_payload_kind::boolean_false)};
        case 'Z': return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::null_fixed)};
        case 'N': return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::noop)};
        case 'U': return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(ubjson_payload_kind::uint8)};
        case 'i': return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(ubjson_payload_kind::int8)};
        case 'I': return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(ubjson_payload_kind::int16)};
        case 'l': return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(ubjson_payload_kind::int32)};
        case 'L': return {slot_index<value_t::number_integer>::value, static_cast<std::uint8_t>(ubjson_payload_kind::int64)};
        case 'u': return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(ubjson_payload_kind::uint16_bjd)};
        case 'm': return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(ubjson_payload_kind::uint32_bjd)};
        case 'M': return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(ubjson_payload_kind::uint64_bjd)};
        case 'h': return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(ubjson_payload_kind::half_float)};
        case 'd': return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(ubjson_payload_kind::float32)};
        case 'D': return {slot_index<value_t::number_float>::value, static_cast<std::uint8_t>(ubjson_payload_kind::float64)};
        case 'C': return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(ubjson_payload_kind::char_)};
        case 'S': return {slot_index<value_t::string>::value, static_cast<std::uint8_t>(ubjson_payload_kind::string_)};
        case 'H': return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::high_precision)};
        case '[': return {slot_index<value_t::array>::value, static_cast<std::uint8_t>(ubjson_payload_kind::array_)};
        case '{': return {slot_index<value_t::object>::value, static_cast<std::uint8_t>(ubjson_payload_kind::object_)};
        case '$': return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::type_marker)};
        case '#': return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::count_marker)};
        case 'B': return {slot_index<value_t::number_unsigned>::value, static_cast<std::uint8_t>(ubjson_payload_kind::byte_bjd)};
        default:  return {0xFF, static_cast<std::uint8_t>(ubjson_payload_kind::invalid)};
    }
}
} // namespace refl_detail

template<std::size_t... I>
consteval auto cbor_load_table_impl(std::index_sequence<I...>)
{
    return std::array<refl_detail::cbor_load_entry, sizeof...(I)>
    {
        refl_detail::cbor_entry_for(static_cast<std::uint8_t>(I))...
    };
}

template<std::size_t... I>
consteval auto msgpack_load_table_impl(std::index_sequence<I...>)
{
    return std::array<refl_detail::msgpack_load_entry, sizeof...(I)>
    {
        refl_detail::msgpack_entry_for(static_cast<std::uint8_t>(I))...
    };
}

template<std::size_t... I>
consteval auto ubjson_load_table_impl(std::index_sequence<I...>)
{
    return std::array<refl_detail::ubjson_load_entry, sizeof...(I)>
    {
        refl_detail::ubjson_entry_for(static_cast<std::uint8_t>(I))...
    };
}

// reverse tables: raw first byte -> load entry (read direction)
constexpr auto kCborLoads =
    cbor_load_table_impl(std::make_index_sequence<256> {});
constexpr auto kMsgpackLoads =
    msgpack_load_table_impl(std::make_index_sequence<256> {});
constexpr auto kUbjsonLoads =
    ubjson_load_table_impl(std::make_index_sequence<256> {});

// table completeness + spot checks: adding a value_t or changing a wire
// mapping is a compile error, never a silent parse drift.
static_assert(kCborLoads.size() == 256 && kMsgpackLoads.size() == 256 && kUbjsonLoads.size() == 256,
              "binary LOAD tables must cover the whole byte range");
static_assert(kCborLoads[0x00].slot == slot_index<value_t::number_unsigned>::value &&
              kCborLoads[0x00].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::uint_immediate), "CBOR uint 0");
static_assert(kCborLoads[0x1B].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::uint64) &&
              kCborLoads[0x3B].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::negint64) &&
              kCborLoads[0x5B].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::bytes64) &&
              kCborLoads[0x7B].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::text64) &&
              kCborLoads[0x9B].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::array64) &&
              kCborLoads[0xBB].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::map64), "CBOR 64-bit rungs");
static_assert(kCborLoads[0x5F].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::bytes_indefinite) &&
              kCborLoads[0x7F].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::text_indefinite) &&
              kCborLoads[0x9F].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::array_indefinite) &&
              kCborLoads[0xBF].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::map_indefinite), "CBOR indefinite forms");
static_assert(kCborLoads[0xD8].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::tag_1byte) &&
              kCborLoads[0xDB].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::tag_8byte) &&
              kCborLoads[0xF4].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::boolean_false) &&
              kCborLoads[0xF6].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::null_fixed) &&
              kCborLoads[0xF9].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::half_float) &&
              kCborLoads[0xFB].kind == static_cast<std::uint8_t>(refl_detail::cbor_payload_kind::float64), "CBOR tag/bool/null/float spots");
static_assert(kMsgpackLoads[0x00].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::positive_fixint) &&
              kMsgpackLoads[0x80].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::fixmap) &&
              kMsgpackLoads[0x90].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::fixarray) &&
              kMsgpackLoads[0xA0].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::fixstr), "MsgPack fix forms");
static_assert(kMsgpackLoads[0xC1].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::invalid) &&
              kMsgpackLoads[0xCA].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::float32) &&
              kMsgpackLoads[0xD0].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::int8) &&
              kMsgpackLoads[0xD4].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::fixext1) &&
              kMsgpackLoads[0xDC].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::array16) &&
              kMsgpackLoads[0xE0].kind == static_cast<std::uint8_t>(refl_detail::msgpack_payload_kind::negative_fixint), "MsgPack code spots");
static_assert(kUbjsonLoads['T'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::boolean_true) &&
              kUbjsonLoads['Z'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::null_fixed) &&
              kUbjsonLoads['U'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::uint8) &&
              kUbjsonLoads['L'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::int64) &&
              kUbjsonLoads['u'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::uint16_bjd) &&
              kUbjsonLoads['M'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::uint64_bjd) &&
              kUbjsonLoads['h'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::half_float) &&
              kUbjsonLoads['S'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::string_) &&
              kUbjsonLoads['H'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::high_precision) &&
              kUbjsonLoads['['].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::array_) &&
              kUbjsonLoads['$'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::type_marker) &&
              kUbjsonLoads['B'].kind == static_cast<std::uint8_t>(refl_detail::ubjson_payload_kind::byte_bjd), "UBJSON marker spots");

// ---------------------------------------------------------------------------
// Reflection-driven MessagePack writer.
//
// Same dispatch principle: value routing from the reflection enumerator set
// (kValueTInfos) via `template for` + per-enumerator NTTP actions
// (msgpack_one<V>); the primary byte codes come from the reflection-generated
// kMsgpackCodes table, the range-dependent forms (fixnum/fixstr/fixarray/
// fixmap, 8/16/32 width selection, ext/fixext binary) are selected inside the
// action. Byte-level encoding replicates nlohmann's write_msgpack so the
// output is byte-identical (differential-tested).
// ---------------------------------------------------------------------------
struct reflection_msgpack_serializer
{
    std::vector<std::uint8_t> out;

    explicit reflection_msgpack_serializer(const basic_json_reflection& j)
    {
        dump(j);
    }

    std::string str() const
    {
        return std::string(out.begin(), out.end());
    }
    const std::vector<std::uint8_t>& bytes() const
    {
        return out;
    }

  private:
    static bool little_endian()
    {
        const uint16_t x = 1;
        return *reinterpret_cast<const uint8_t*>(&x) == 1;
    }

    template<typename T>
    void append_big(T v)
    {
        const auto n = static_cast<std::size_t>(sizeof(T));
        std::array<uint8_t, sizeof(T)> tmp{};
        std::memcpy(tmp.data(), &v, n);
        if (little_endian())
        {
            std::reverse(tmp.begin(), tmp.end());
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    }

    void dump(const basic_json_reflection& j)
    {
        const value_t t = j.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                msgpack_one<V>(j);
            }
        }
    }

    template<value_t V>
    void msgpack_one(const basic_json_reflection& j)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::null)
        {
            out.push_back(0xC0);
        }
        else if constexpr (V == value_t::boolean)
        {
            out.push_back(u.boolean ? 0xC3 : 0xC2);
        }
        else if constexpr (V == value_t::number_integer)
        {
            if (u.number_integer >= 0)
            {
                // MessagePack does not differentiate positive signed from
                // unsigned integers — same code as number_unsigned
                msgpack_unsigned(static_cast<std::uint64_t>(u.number_integer));
            }
            else
            {
                if (u.number_integer >= -32)
                {
                    // negative fixnum
                    out.push_back(static_cast<std::uint8_t>(u.number_integer));
                }
                else if (u.number_integer >= (std::numeric_limits<std::int8_t>::min)())
                {
                    out.push_back(0xD0);
                    out.push_back(static_cast<std::uint8_t>(u.number_integer));
                }
                else if (u.number_integer >= (std::numeric_limits<std::int16_t>::min)())
                {
                    out.push_back(0xD1);
                    append_big(static_cast<std::int16_t>(u.number_integer));
                }
                else if (u.number_integer >= (std::numeric_limits<std::int32_t>::min)())
                {
                    out.push_back(0xD2);
                    append_big(static_cast<std::int32_t>(u.number_integer));
                }
                else
                {
                    out.push_back(0xD3);
                    append_big(static_cast<std::int64_t>(u.number_integer));
                }
            }
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            msgpack_unsigned(u.number_unsigned);
        }
        else if constexpr (V == value_t::number_float)
        {
            msgpack_float(u.number_float);
        }
        else if constexpr (V == value_t::string)
        {
            msgpack_string(*u.string);
        }
        else if constexpr (V == value_t::array)
        {
            msgpack_array(*u.array);
        }
        else if constexpr (V == value_t::object)
        {
            msgpack_object(*u.object);
        }
        else if constexpr (V == value_t::binary)
        {
            msgpack_binary(*u.binary);
        }
        // null handled above; discarded: nothing (matches the library's
        // default: break)
    }

    void msgpack_unsigned(const std::uint64_t n)
    {
        if (n < 128)
        {
            out.push_back(static_cast<std::uint8_t>(n)); // positive fixnum
        }
        else if (n <= (std::numeric_limits<std::uint8_t>::max)())
        {
            out.push_back(0xCC);
            out.push_back(static_cast<std::uint8_t>(n));
        }
        else if (n <= (std::numeric_limits<std::uint16_t>::max)())
        {
            out.push_back(0xCD);
            append_big(static_cast<std::uint16_t>(n));
        }
        else if (n <= (std::numeric_limits<std::uint32_t>::max)())
        {
            out.push_back(0xCE);
            append_big(static_cast<std::uint32_t>(n));
        }
        else
        {
            out.push_back(0xCF);
            append_big(static_cast<std::uint64_t>(n));
        }
    }

    // replicate the library's write_compact_float (msgpack branch): float32
    // when non-finite or exactly representable in float, else float64
    void msgpack_float(const double n)
    {
        constexpr double F_MIN = static_cast<double>((std::numeric_limits<float>::lowest)());
        constexpr double F_MAX = static_cast<double>((std::numeric_limits<float>::max)());
        const bool use_float =
            !std::isfinite(n) ||
            (n >= F_MIN && n <= F_MAX &&
             static_cast<double>(static_cast<float>(n)) == n);
        if (use_float)
        {
            out.push_back(0xCA);
            append_big(static_cast<float>(n));
        }
        else
        {
            out.push_back(0xCB);
            append_big(n);
        }
    }

    void msgpack_string(const std::string& s)
    {
        const auto N = s.size();
        if (N <= 31)
        {
            out.push_back(static_cast<std::uint8_t>(0xA0 | N)); // fixstr
        }
        else if (N <= (std::numeric_limits<std::uint8_t>::max)())
        {
            out.push_back(0xD9);
            out.push_back(static_cast<std::uint8_t>(N));
        }
        else if (N <= (std::numeric_limits<std::uint16_t>::max)())
        {
            out.push_back(0xDA);
            append_big(static_cast<std::uint16_t>(N));
        }
        else
        {
            out.push_back(0xDB);
            append_big(static_cast<std::uint32_t>(N));
        }
        out.insert(out.end(), s.begin(), s.end());
    }

    void msgpack_array(const json::array_t& arr)
    {
        const auto N = arr.size();
        if (N <= 15)
        {
            out.push_back(static_cast<std::uint8_t>(0x90 | N)); // fixarray
        }
        else if (N <= (std::numeric_limits<std::uint16_t>::max)())
        {
            out.push_back(0xDC);
            append_big(static_cast<std::uint16_t>(N));
        }
        else
        {
            out.push_back(0xDD);
            append_big(static_cast<std::uint32_t>(N));
        }
        for (const auto& el : arr)
        {
            basic_json_reflection val;
            val.assign_from(el);
            dump(val);
        }
    }

    void msgpack_object(const json::object_t& obj)
    {
        const auto N = obj.size();
        if (N <= 15)
        {
            out.push_back(static_cast<std::uint8_t>(0x80 | (N & 0xF))); // fixmap
        }
        else if (N <= (std::numeric_limits<std::uint16_t>::max)())
        {
            out.push_back(0xDE);
            append_big(static_cast<std::uint16_t>(N));
        }
        else
        {
            out.push_back(0xDF);
            append_big(static_cast<std::uint32_t>(N));
        }
        for (const auto& el : obj)
        {
            msgpack_string(el.first); // MessagePack map keys are strings
            basic_json_reflection val;
            val.assign_from(el.second);
            dump(val);
        }
    }

    void msgpack_binary(const json::binary_t& bin)
    {
        const bool use_ext = bin.has_subtype();
        const auto N = bin.size();
        if (N <= (std::numeric_limits<std::uint8_t>::max)())
        {
            std::uint8_t output_type{};
            bool fixed = true;
            if (use_ext)
            {
                switch (N)
                {
                    case 1:
                        output_type = 0xD4; // fixext 1
                        break;
                    case 2:
                        output_type = 0xD5; // fixext 2
                        break;
                    case 4:
                        output_type = 0xD6; // fixext 4
                        break;
                    case 8:
                        output_type = 0xD7; // fixext 8
                        break;
                    case 16:
                        output_type = 0xD8; // fixext 16
                        break;
                    default:
                        output_type = 0xC7; // ext 8
                        fixed = false;
                        break;
                }
            }
            else
            {
                output_type = 0xC4; // bin 8
                fixed = false;
            }
            out.push_back(output_type);
            if (!fixed)
            {
                out.push_back(static_cast<std::uint8_t>(N));
            }
        }
        else if (N <= (std::numeric_limits<std::uint16_t>::max)())
        {
            out.push_back(use_ext ? 0xC8 : 0xC5); // ext 16 / bin 16
            append_big(static_cast<std::uint16_t>(N));
        }
        else
        {
            out.push_back(use_ext ? 0xC9 : 0xC6); // ext 32 / bin 32
            append_big(static_cast<std::uint32_t>(N));
        }
        if (use_ext)
        {
            out.push_back(static_cast<std::uint8_t>(bin.subtype()));
        }
        out.insert(out.end(), bin.begin(), bin.end());
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven UBJSON writer (no-optimization mode: use_count=false,
// use_type=false, use_bjdata=false — the library's to_ubjson defaults).
//
// Primary byte codes from the reflection-generated kUbjsonCodes table;
// number narrowing ('i'/'U'/'I'/'l'/'L', 'H' high-precision) is selected
// inside the action. Byte-level encoding replicates nlohmann's write_ubjson
// in the no-optimization mode so the output is byte-identical
// (differential-tested). The optimized modes ('#'/'\$' prefixes, BJData) are
// deliberately not implemented — see the coverage note in the docs.
// ---------------------------------------------------------------------------
struct reflection_ubjson_serializer
{
    std::vector<std::uint8_t> out;

    explicit reflection_ubjson_serializer(const basic_json_reflection& j)
    {
        dump(j);
    }

    std::string str() const
    {
        return std::string(out.begin(), out.end());
    }
    const std::vector<std::uint8_t>& bytes() const
    {
        return out;
    }

  private:
    static bool little_endian()
    {
        const uint16_t x = 1;
        return *reinterpret_cast<const uint8_t*>(&x) == 1;
    }

    template<typename T>
    void append_big(T v)
    {
        const auto n = static_cast<std::size_t>(sizeof(T));
        std::array<uint8_t, sizeof(T)> tmp{};
        std::memcpy(tmp.data(), &v, n);
        if (little_endian())
        {
            std::reverse(tmp.begin(), tmp.end());
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    }

    void dump(const basic_json_reflection& j)
    {
        const value_t t = j.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                ubjson_one<V>(j);
            }
        }
    }

    template<value_t V>
    void ubjson_one(const basic_json_reflection& j)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::null)
        {
            out.push_back('Z');
        }
        else if constexpr (V == value_t::boolean)
        {
            out.push_back(u.boolean ? 'T' : 'F');
        }
        else if constexpr (V == value_t::number_integer)
        {
            ubjson_signed(u.number_integer);
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            ubjson_unsigned(u.number_unsigned);
        }
        else if constexpr (V == value_t::number_float)
        {
            // UBJSON floats are NOT compacted: number_float_t is double,
            // so the prefix is always 'D' + float64 (get_ubjson_float_prefix)
            out.push_back('D');
            append_big(u.number_float);
        }
        else if constexpr (V == value_t::string)
        {
            out.push_back('S');
            ubjson_unsigned(static_cast<std::uint64_t>(u.string->size()));
            out.insert(out.end(), u.string->begin(), u.string->end());
        }
        else if constexpr (V == value_t::array)
        {
            out.push_back('[');
            for (const auto& el : *u.array)
            {
                basic_json_reflection val;
                val.assign_from(el);
                dump(val);
            }
            out.push_back(']');
        }
        else if constexpr (V == value_t::object)
        {
            out.push_back('{');
            for (const auto& el : *u.object)
            {
                ubjson_unsigned(static_cast<std::uint64_t>(el.first.size()));
                out.insert(out.end(), el.first.begin(), el.first.end());
                basic_json_reflection val;
                val.assign_from(el.second);
                dump(val);
            }
            out.push_back('}');
        }
        else if constexpr (V == value_t::binary)
        {
            out.push_back('[');
            // no-optimization mode: each byte with a 'U' prefix
            for (const auto b : *u.binary)
            {
                out.push_back('U');
                out.push_back(b);
            }
            out.push_back(']');
        }
        // discarded: nothing (matches the library's default: break)
    }

    // UBJSON number narrowing — two ladders replicated from
    // write_number_with_ubjson_prefix (use_bjdata=false): the signed ladder
    // (with an explicit 0 <= n <= u8_max middle rung — a negative value must
    // never take the 'U' branch) and the unsigned ladder (values above
    // int64 max fall to 'H' high-precision instead of overflowing the
    // int64 conversion).
    void ubjson_signed(const std::int64_t n)
    {
        if ((std::numeric_limits<std::int8_t>::min)() <= n && n <= (std::numeric_limits<std::int8_t>::max)())
        {
            out.push_back('i');
            out.push_back(static_cast<std::uint8_t>(n));
        }
        else if (0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint8_t>::max)()))
        {
            out.push_back('U');
            out.push_back(static_cast<std::uint8_t>(n));
        }
        else if ((std::numeric_limits<std::int16_t>::min)() <= n && n <= (std::numeric_limits<std::int16_t>::max)())
        {
            out.push_back('I');
            append_big(static_cast<std::int16_t>(n));
        }
        else if ((std::numeric_limits<std::int32_t>::min)() <= n && n <= (std::numeric_limits<std::int32_t>::max)())
        {
            out.push_back('l');
            append_big(static_cast<std::int32_t>(n));
        }
        else
        {
            out.push_back('L');
            append_big(static_cast<std::int64_t>(n));
        }
    }

    void ubjson_unsigned(const std::uint64_t n)
    {
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int8_t>::max)()))
        {
            out.push_back('i');
            out.push_back(static_cast<std::uint8_t>(n));
        }
        else if (n <= (std::numeric_limits<std::uint8_t>::max)())
        {
            out.push_back('U');
            out.push_back(static_cast<std::uint8_t>(n));
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::max)()))
        {
            out.push_back('I');
            append_big(static_cast<std::int16_t>(n));
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))
        {
            out.push_back('l');
            append_big(static_cast<std::int32_t>(n));
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            out.push_back('L');
            append_big(static_cast<std::int64_t>(n));
        }
        else
        {
            // high-precision number: 'H' + length + decimal-string dump
            out.push_back('H');
            const std::string num = json(n).dump();
            ubjson_unsigned(static_cast<std::uint64_t>(num.size()));
            out.insert(out.end(), num.begin(), num.end());
        }
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven BSON writer.
//
// BSON is a document stream: the top-level value MUST be an object (the
// library throws type_error 317 otherwise — replicated here), and every
// element is [type-byte][name][0x00][value] with embedded little-endian
// int32 length fields (documents, strings, arrays, binaries) computed by a
// size pre-pass. Element type bytes come from the reflection-generated
// kBsonCodes table; integer narrowing (int32/int64/uint64) is selected
// inside the action. Byte-level encoding replicates nlohmann's write_bson so
// the output is byte-identical (differential-tested).
// ---------------------------------------------------------------------------
struct reflection_bson_serializer
{
    std::vector<std::uint8_t> out;

    explicit reflection_bson_serializer(const basic_json_reflection& j)
    {
        write_bson(j);
    }

    std::string str() const
    {
        return std::string(out.begin(), out.end());
    }
    const std::vector<std::uint8_t>& bytes() const
    {
        return out;
    }

  private:
    static bool little_endian()
    {
        const uint16_t x = 1;
        return *reinterpret_cast<const uint8_t*>(&x) == 1;
    }

    // little-endian (BSON)
    template<typename T>
    void append_little(T v)
    {
        const auto n = static_cast<std::size_t>(sizeof(T));
        std::array<uint8_t, sizeof(T)> tmp{};
        std::memcpy(tmp.data(), &v, n);
        if (!little_endian())
        {
            std::reverse(tmp.begin(), tmp.end());
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    }

    static std::int32_t to_bson_length(const std::size_t size)
    {
        return static_cast<std::int32_t>(size);
    }

    void write_bson(const basic_json_reflection& j)
    {
        if (j.is_object())
        {
            bson_object(*j.value().object);
            return;
        }
        throw std::runtime_error("to serialize to BSON, top-level type must be object, but is " +
                                 std::string(j.type() == value_t::null ? "null" : "not object"));
    }

    void bson_object(const json::object_t& obj)
    {
        append_little(to_bson_length(calc_object_size(obj)));
        for (const auto& el : obj)
        {
            bson_element(el.first, el.second);
        }
        out.push_back(0x00);
    }

    void bson_entry_header(const std::string& name, const std::uint8_t element_type)
    {
        out.push_back(element_type);
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0x00);
    }

    void bson_element(const std::string& name, const json& j)
    {
        basic_json_reflection val;
        val.assign_from(j);
        const value_t t = val.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                bson_one<V>(name, val);
            }
        }
    }

    template<value_t V>
    void bson_one(const std::string& name, const basic_json_reflection& j)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::object)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::object>::value]);
            bson_object(*u.object);
        }
        else if constexpr (V == value_t::array)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::array>::value]);
            append_little(to_bson_length(calc_array_size(*u.array)));
            std::size_t idx = 0;
            for (const auto& el : *u.array)
            {
                bson_element(std::to_string(idx++), el);
            }
            out.push_back(0x00);
        }
        else if constexpr (V == value_t::string)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::string>::value]);
            append_little(to_bson_length(u.string->size() + 1ul));
            out.insert(out.end(), u.string->begin(), u.string->end());
            out.push_back(0x00);
        }
        else if constexpr (V == value_t::binary)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::binary>::value]);
            append_little(to_bson_length(u.binary->size()));
            out.push_back(u.binary->has_subtype() ? static_cast<std::uint8_t>(u.binary->subtype())
                                                  : static_cast<std::uint8_t>(0x00));
            out.insert(out.end(), u.binary->begin(), u.binary->end());
        }
        else if constexpr (V == value_t::boolean)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::boolean>::value]);
            out.push_back(u.boolean ? 0x01 : 0x00);
        }
        else if constexpr (V == value_t::null)
        {
            // null has no union slot — its BSON element type is fixed 0x0A
            bson_entry_header(name, 0x0A);
        }
        else if constexpr (V == value_t::number_integer)
        {
            const auto n = static_cast<std::int64_t>(u.number_integer);
            if ((std::numeric_limits<std::int32_t>::min)() <= n && n <= (std::numeric_limits<std::int32_t>::max)())
            {
                bson_entry_header(name, 0x10); // int32
                append_little(static_cast<std::int32_t>(n));
            }
            else
            {
                bson_entry_header(name, 0x12); // int64
                append_little(static_cast<std::int64_t>(n));
            }
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            const auto n = static_cast<std::uint64_t>(u.number_unsigned);
            if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))
            {
                bson_entry_header(name, 0x10); // int32
                append_little(static_cast<std::int32_t>(n));
            }
            else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            {
                bson_entry_header(name, 0x12); // int64
                append_little(static_cast<std::int64_t>(n));
            }
            else
            {
                bson_entry_header(name, 0x11); // uint64
                append_little(static_cast<std::uint64_t>(n));
            }
        }
        else if constexpr (V == value_t::number_float)
        {
            bson_entry_header(name, kBsonCodes[slot_index<value_t::number_float>::value]);
            append_little(u.number_float);
        }
        // discarded: nothing (library's default JSON_ASSERT in debug, silent in release)
    }

    // --- size pre-pass (mirrors calc_bson_*_size) --------------------------
    static std::size_t calc_element_size(const std::string& name, const json& j)
    {
        basic_json_reflection val;
        val.assign_from(j);
        const value_t t = val.type();
        switch (t)
        {
            case value_t::object:
                return name.size() + 2ul + calc_object_size(*val.value().object);
            case value_t::array:
                return name.size() + 2ul + calc_array_size(*val.value().array);
            case value_t::string:
                return name.size() + 2ul + sizeof(std::int32_t) + val.value().string->size() + 1ul;
            case value_t::binary:
                return name.size() + 2ul + sizeof(std::int32_t) + 1ul + val.value().binary->size();
            case value_t::boolean:
                return name.size() + 2ul + 1ul;
            case value_t::null:
                return name.size() + 2ul;
            case value_t::number_integer:
            {
                const auto n = static_cast<std::int64_t>(val.value().number_integer);
                return name.size() + 2ul +
                       ((std::numeric_limits<std::int32_t>::min)() <= n && n <= (std::numeric_limits<std::int32_t>::max)()
                        ? sizeof(std::int32_t) : sizeof(std::int64_t));
            }
            case value_t::number_unsigned:
            {
                const auto n = static_cast<std::uint64_t>(val.value().number_unsigned);
                return name.size() + 2ul +
                       (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
                        ? sizeof(std::int32_t)
                        : (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
                           ? sizeof(std::int64_t) : sizeof(std::uint64_t)));
            }
            case value_t::number_float:
                return name.size() + 2ul + sizeof(double);
            case value_t::discarded:
            default:
                return 0ul;
        }
    }

    static std::size_t calc_object_size(const json::object_t& obj)
    {
        std::size_t s = 0;
        for (const auto& el : obj)
        {
            s += calc_element_size(el.first, el.second);
        }
        return sizeof(std::int32_t) + s + 1ul;
    }

    static std::size_t calc_array_size(const json::array_t& arr)
    {
        std::size_t s = 0;
        std::size_t idx = 0;
        for (const auto& el : arr)
        {
            s += calc_element_size(std::to_string(idx++), el);
        }
        return sizeof(std::int32_t) + s + 1ul;
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven BSON reader.
//
// Mirror image of reflection_bson_serializer: bytes -> basic_json_reflection.
// The element-type dispatch comes from the reflection-generated kBsonsLoad
// table (byte code -> {union slot, payload kind}); the payload kind selects
// the reading action. Document framing (int32 size, 0x00 terminator, cstr
// names, length prefixes) and error handling are plain mechanics — the
// reflection contribution is the routing table plus its compile-time
// completeness guarantee, exactly as on the writer side.
//
// Semantics mirror nlohmann's from_bson (verified against
// detail/input/binary_reader.hpp): top-level value must be an object;
// strings carry [int32 len][len-1 bytes][0x00]; binaries [int32 len][1-byte
// subtype][len bytes]; arrays are documents whose keys are ignored
// (position-based, matching the library); the declared document size must
// equal the bytes consumed. Errors return false + a diagnostic (this study
// library has no exception machinery); differential testing covers legal
// input only — malformed input is guaranteed to fail safely, not to match
// the library's exact error codes.
// ---------------------------------------------------------------------------
struct reflection_bson_parser
{
    std::string err;

    explicit reflection_bson_parser(const std::vector<std::uint8_t>& in)
        : data(in)
    {}

    bool parse(basic_json_reflection& out)
    {
        pos = 0;
        json top;
        if (!parse_document(false, top))
        {
            return false;
        }
        if (!top.is_object())
        {
            err = "to deserialize from BSON, top-level type must be object";
            return false;
        }
        out.assign_from(top);
        return true;
    }

    const std::string& error() const
    {
        return err;
    }

  private:
    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;

    bool fail(const char* what)
    {
        if (err.empty())
        {
            err = what;
        }
        return false;
    }

    bool at_end() const
    {
        return pos >= data.size();
    }

    bool take(std::uint8_t& b)
    {
        if (at_end())
        {
            return fail("unexpected end of input");
        }
        b = data[pos++];
        return true;
    }

    // little-endian fixed-width read (same semantics as the library's get_number)
    template<typename T>
    bool read_le(T& out)
    {
        if (data.size() - pos < sizeof(T))
        {
            return fail("truncated number");
        }
        T v{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            v |= static_cast<T>(data[pos + i]) << (8 * i);
        }
        pos += sizeof(T);
        out = v;
        return true;
    }

    bool read_cstr(std::string& out)
    {
        const std::size_t start = pos;
        while (true)
        {
            if (at_end())
            {
                return fail("unterminated cstring");
            }
            if (data[pos++] == 0x00)
            {
                break;
            }
        }
        out.assign(reinterpret_cast<const char*>(data.data()) + start, pos - start - 1);
        return true;
    }

    // [int32 document_size][elements...][0x00]; element = [type][name][0x00][value]
    bool parse_document(const bool is_array, json& out)
    {
        const std::size_t doc_start = pos;
        std::int32_t doc_size{};
        if (!read_le(doc_size) || doc_size < 0)
        {
            return fail("invalid document size");
        }

        json::array_t arr;
        json::object_t obj;
        while (true)
        {
            std::uint8_t code{};
            if (!take(code))
            {
                return fail("unexpected end of document");
            }
            if (code == 0x00)
            {
                break; // document terminator
            }
            std::string name;
            if (!read_cstr(name))
            {
                return false;
            }
            json val;
            if (!parse_element(code, val))
            {
                return false;
            }
            if (is_array)
            {
                arr.push_back(std::move(val)); // BSON array keys are ignored (position-based)
            }
            else
            {
                obj.emplace(std::move(name), std::move(val));
            }
        }

        if (static_cast<std::size_t>(doc_size) != pos - doc_start)
        {
            return fail("document size does not match bytes read");
        }

        if (is_array)
        {
            out = std::move(arr);
        }
        else
        {
            out = std::move(obj);
        }
        return true;
    }

    // payload dispatch: routing comes from the reflection-generated kBsonsLoad
    // table; each case below is the reading mechanism for one payload kind.
    bool parse_element(const std::uint8_t code, json& out)
    {
        const refl_detail::bson_load_entry entry = kBsonsLoad[code];
        switch (static_cast<refl_detail::bson_payload_kind>(entry.kind))
        {
            case refl_detail::bson_payload_kind::double_fixed:
            {
                // little-endian double via byte assembly (no shift on double)
                if (data.size() - pos < sizeof(double))
                {
                    return fail("truncated number");
                }
                std::uint64_t bits = 0;
                for (std::size_t i = 0; i < sizeof(double); ++i)
                {
                    bits |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i);
                }
                pos += sizeof(double);
                double v{};
                std::memcpy(&v, &bits, sizeof(double));
                out = v;
                return true;
            }
            case refl_detail::bson_payload_kind::string_len:
            {
                std::int32_t len{};
                if (!read_le(len) || len < 1)
                {
                    return fail("invalid string length");
                }
                const auto n = static_cast<std::size_t>(len);
                if (data.size() - pos < n)
                {
                    return fail("truncated string");
                }
                std::string s(reinterpret_cast<const char*>(data.data()) + pos, n - 1);
                pos += n;
                out = std::move(s);
                return true;
            }
            case refl_detail::bson_payload_kind::object_doc:
                return parse_document(false, out);
            case refl_detail::bson_payload_kind::array_doc:
                return parse_document(true, out);
            case refl_detail::bson_payload_kind::binary_len:
            {
                std::int32_t len{};
                if (!read_le(len) || len < 0)
                {
                    return fail("invalid binary length");
                }
                std::uint8_t subtype{};
                if (!take(subtype))
                {
                    return false;
                }
                const auto n = static_cast<std::size_t>(len);
                if (data.size() - pos < n)
                {
                    return fail("truncated binary");
                }
                // byte_container_with_subtype has no iterator-pair ctor —
                // build the container first, then the binary wrapper
                std::vector<std::uint8_t> raw(data.begin() + pos, data.begin() + pos + n);
                json::binary_t bin(std::move(raw));
                bin.set_subtype(subtype);
                pos += n;
                out = std::move(bin);
                return true;
            }
            case refl_detail::bson_payload_kind::boolean_byte:
            {
                std::uint8_t b{};
                return take(b) && (out = (b != 0), true);
            }
            case refl_detail::bson_payload_kind::null_fixed:
                out = nullptr;
                return true;
            case refl_detail::bson_payload_kind::int32_le:
            {
                std::int32_t v{};
                return read_le(v) && (out = static_cast<json::number_integer_t>(v), true);
            }
            case refl_detail::bson_payload_kind::int64_le:
            {
                std::int64_t v{};
                return read_le(v) && (out = v, true);
            }
            case refl_detail::bson_payload_kind::uint64_le:
            {
                std::uint64_t v{};
                return read_le(v) && (out = v, true);
            }
            default:
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", code);
                err = std::string("unsupported BSON element type ") + hex;
                return false;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven UBJSON writer, OPTIMIZED MODES + BJData (M4E).
//
// Superset of reflection_ubjson_serializer: adds the '#' count prefix
// (use_count), the '$' type prefix (use_type — requires use_count, as in the
// library), the top-level-prefix control (add_prefix), and the BJData dialect
// (use_bjdata). Number routing still comes from the reflection enumerator set
// via `template for` + per-enumerator NTTP actions; the byte-code table
// (kUbjsonCodes) stays the single source of truth for the primary codes, the
// optimized-mode extensions (count/type prefixes, 'u'/'m'/'M' BJData rungs,
// per-element prefixes) are selected inside the actions — exactly the same
// division of labor as the no-optimization writer.
//
// BJData specifics replicated from binary_writer.hpp write_ubjson:
//   * ALL numbers and length prefixes are LITTLE-endian (write_number(n,
//     OutputIsLittleEndian=use_bjdata)) — not just the unsigned types;
//   * the unsigned ladders gain 'u' (uint16) / 'm' (uint32) / 'M' (uint64)
//     rungs between the signed rungs;
//   * the '$' type optimization EXCLUDES the markers [ '[' '{' 'S' 'H' 'T'
//     'F' 'N' 'Z' ] (bjdx list) — such containers fall back to per-element
//     prefixes;
//   * an object with exactly {_ArrayType_, _ArraySize_, _ArrayData_} is
//     encoded as a JData ndarray ([$<dtype>#<size-array> <compact elements>])
//     when the dtype/size/element-kind checks pass, else falls back to a
//     plain object;
//   * draft3 (bjdata_version_t::draft3) encodes binary with the 'B' marker
//     instead of 'U'.
// Byte-level output is differential-tested byte-identical against the real
// library's to_ubjson / to_bjdata (m4e_ubjson_opt.cpp).
// ---------------------------------------------------------------------------
struct reflection_ubjson_optimized_serializer
{
    std::vector<std::uint8_t> out;

    explicit reflection_ubjson_optimized_serializer(
        const basic_json_reflection& j,
        const bool use_count_,
        const bool use_type_,
        const bool use_bjdata_ = false,
        const bool bjdata_draft3_ = false)
        : use_count(use_count_),
          use_type(use_type_),
          use_bjdata(use_bjdata_),
          bjdata_draft3(use_bjdata_ && bjdata_draft3_)
    {
        write(j, true); // top level always carries its prefix
    }

    const std::vector<std::uint8_t>& bytes() const
    {
        return out;
    }

  private:
    const bool use_count;
    const bool use_type;
    const bool use_bjdata;
    const bool bjdata_draft3;

    static bool host_little_endian()
    {
        const uint16_t x = 1;
        return *reinterpret_cast<const uint8_t*>(&x) == 1;
    }

    // endian-aware fixed-width number: little=true for BJData (all numbers
    // and lengths are little-endian there), big-endian otherwise — the
    // library's write_number(n, OutputIsLittleEndian=use_bjdata)
    template<typename T>
    void write_num(const T v, const bool little)
    {
        std::array<uint8_t, sizeof(T)> tmp{};
        std::memcpy(tmp.data(), &v, sizeof(T));
        if (host_little_endian() != little)
        {
            std::reverse(tmp.begin(), tmp.end());
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    }

    // --- number ladders (binary_writer.hpp write_number_with_ubjson_prefix;
    // use_bjdata adds the 'u'/'m'/'M' rungs, everything little-endian) ---
    void ubjson_signed(const std::int64_t n, const bool add_prefix)
    {
        if ((std::numeric_limits<std::int8_t>::min)() <= n && n <= (std::numeric_limits<std::int8_t>::max)())
        {
            if (add_prefix) out.push_back('i');
            write_num(static_cast<std::int8_t>(n), use_bjdata);
        }
        else if (0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint8_t>::max)()))
        {
            if (add_prefix) out.push_back('U');
            write_num(static_cast<std::uint8_t>(n), use_bjdata);
        }
        else if ((std::numeric_limits<std::int16_t>::min)() <= n && n <= (std::numeric_limits<std::int16_t>::max)())
        {
            if (add_prefix) out.push_back('I');
            write_num(static_cast<std::int16_t>(n), use_bjdata);
        }
        else if (use_bjdata && 0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint16_t>::max)()))
        {
            if (add_prefix) out.push_back('u'); // uint16 - bjdata only
            write_num(static_cast<std::uint16_t>(n), use_bjdata);
        }
        else if ((std::numeric_limits<std::int32_t>::min)() <= n && n <= (std::numeric_limits<std::int32_t>::max)())
        {
            if (add_prefix) out.push_back('l');
            write_num(static_cast<std::int32_t>(n), use_bjdata);
        }
        else if (use_bjdata && 0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)()))
        {
            if (add_prefix) out.push_back('m'); // uint32 - bjdata only
            write_num(static_cast<std::uint32_t>(n), use_bjdata);
        }
        else if ((std::numeric_limits<std::int64_t>::min)() <= n && n <= (std::numeric_limits<std::int64_t>::max)())
        {
            if (add_prefix) out.push_back('L');
            write_num(static_cast<std::int64_t>(n), use_bjdata);
        }
        else
        {
            // high-precision number (unreachable for int64 in practice)
            if (add_prefix) out.push_back('H');
            high_precision(n);
        }
    }

    void ubjson_unsigned(const std::uint64_t n, const bool add_prefix)
    {
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int8_t>::max)()))
        {
            if (add_prefix) out.push_back('i');
            write_num(static_cast<std::uint8_t>(n), use_bjdata);
        }
        else if (n <= (std::numeric_limits<std::uint8_t>::max)())
        {
            if (add_prefix) out.push_back('U');
            write_num(static_cast<std::uint8_t>(n), use_bjdata);
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::max)()))
        {
            if (add_prefix) out.push_back('I');
            write_num(static_cast<std::int16_t>(n), use_bjdata);
        }
        else if (use_bjdata && n <= static_cast<std::uint64_t>((std::numeric_limits<std::uint16_t>::max)()))
        {
            if (add_prefix) out.push_back('u'); // uint16 - bjdata only
            write_num(static_cast<std::uint16_t>(n), use_bjdata);
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))
        {
            if (add_prefix) out.push_back('l');
            write_num(static_cast<std::int32_t>(n), use_bjdata);
        }
        else if (use_bjdata && n <= static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()))
        {
            if (add_prefix) out.push_back('m'); // uint32 - bjdata only
            write_num(static_cast<std::uint32_t>(n), use_bjdata);
        }
        else if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            if (add_prefix) out.push_back('L');
            write_num(static_cast<std::int64_t>(n), use_bjdata);
        }
        else if (use_bjdata && n <= (std::numeric_limits<std::uint64_t>::max)())
        {
            if (add_prefix) out.push_back('M'); // uint64 - bjdata only
            write_num(static_cast<std::uint64_t>(n), use_bjdata);
        }
        else
        {
            // high-precision number: uint64 above int64 max (plain UBJSON)
            if (add_prefix) out.push_back('H');
            high_precision(n);
        }
    }

    // 'H' + length-prefixed decimal dump (the library: json(n).dump())
    template<typename T>
    void high_precision(const T n)
    {
        char tmp[32];
        const auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), n);
        ubjson_unsigned(static_cast<std::uint64_t>(p - tmp), true);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(tmp), reinterpret_cast<const uint8_t*>(p));
    }

    void ubjson_float(const double n, const bool add_prefix)
    {
        // number_float_t is double: the prefix is always 'D' + float64
        // (UBJSON floats are NOT compacted — verified fact)
        if (add_prefix) out.push_back('D');
        write_num(n, use_bjdata);
    }

    // --- the element-type prefix for '$' type optimization (binary_writer.hpp
    // ubjson_prefix) ---
    char prefix_of(const json& j) const noexcept
    {
        if (j.is_null())
        {
            return 'Z';
        }
        if (j.is_boolean())
        {
            return j.get<bool>() ? 'T' : 'F';
        }
        if (j.is_number_unsigned())
        {
            return prefix_unsigned(j.get<std::uint64_t>());
        }
        if (j.is_number_integer())
        {
            return prefix_signed(j.get<std::int64_t>());
        }
        if (j.is_number_float())
        {
            return 'D';
        }
        if (j.is_string())
        {
            return 'S';
        }
        if (j.is_array() || j.is_binary())
        {
            return '[';
        }
        if (j.is_object())
        {
            return '{';
        }
        return 'N'; // discarded
    }

    char prefix_signed(const std::int64_t n) const noexcept
    {
        if ((std::numeric_limits<std::int8_t>::min)() <= n && n <= (std::numeric_limits<std::int8_t>::max)()) return 'i';
        if (0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint8_t>::max)())) return 'U';
        if ((std::numeric_limits<std::int16_t>::min)() <= n && n <= (std::numeric_limits<std::int16_t>::max)()) return 'I';
        if (use_bjdata && 0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint16_t>::max)())) return 'u';
        if ((std::numeric_limits<std::int32_t>::min)() <= n && n <= (std::numeric_limits<std::int32_t>::max)()) return 'l';
        if (use_bjdata && 0 <= n && n <= static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) return 'm';
        if ((std::numeric_limits<std::int64_t>::min)() <= n && n <= (std::numeric_limits<std::int64_t>::max)()) return 'L';
        return 'H';
    }

    char prefix_unsigned(const std::uint64_t n) const noexcept
    {
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int8_t>::max)())) return 'i';
        if (n <= (std::numeric_limits<std::uint8_t>::max)()) return 'U';
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::max)())) return 'I';
        if (use_bjdata && n <= static_cast<std::uint64_t>((std::numeric_limits<std::uint16_t>::max)())) return 'u';
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) return 'l';
        if (use_bjdata && n <= static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) return 'm';
        if (n <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return 'L';
        if (use_bjdata && n <= (std::numeric_limits<std::uint64_t>::max)()) return 'M';
        return 'H';
    }

    // wrap a real json element into a reflection and write it (the recursion
    // carrier, same as the other writers)
    void write_element(const json& el, const bool add_prefix)
    {
        basic_json_reflection val;
        val.assign_from(el);
        write(val, add_prefix);
    }

    // --- main dispatch: `template for` over the enumerator set ---
    void write(const basic_json_reflection& j, const bool add_prefix)
    {
        const value_t t = j.type();
        template for (constexpr auto r : kValueTInfos)
        {
            constexpr value_t V = static_cast<value_t>([: r :]);
            if (t == V)
            {
                write_one<V>(j, add_prefix);
            }
        }
    }

    template<value_t V>
    void write_one(const basic_json_reflection& j, const bool add_prefix)
    {
        const auto& u = j.value();
        if constexpr (V == value_t::null)
        {
            if (add_prefix) out.push_back('Z');
        }
        else if constexpr (V == value_t::boolean)
        {
            if (add_prefix) out.push_back(u.boolean ? 'T' : 'F');
        }
        else if constexpr (V == value_t::number_integer)
        {
            ubjson_signed(u.number_integer, add_prefix);
        }
        else if constexpr (V == value_t::number_unsigned)
        {
            ubjson_unsigned(u.number_unsigned, add_prefix);
        }
        else if constexpr (V == value_t::number_float)
        {
            ubjson_float(u.number_float, add_prefix);
        }
        else if constexpr (V == value_t::string)
        {
            if (add_prefix) out.push_back('S');
            ubjson_unsigned(static_cast<std::uint64_t>(u.string->size()), true);
            out.insert(out.end(), u.string->begin(), u.string->end());
        }
        else if constexpr (V == value_t::array)
        {
            write_array(*u.array, add_prefix);
        }
        else if constexpr (V == value_t::binary)
        {
            write_binary(*u.binary, add_prefix);
        }
        else if constexpr (V == value_t::object)
        {
            write_object(*u.object, add_prefix);
        }
        // discarded: nothing (the library's default: break)
    }

    void write_array(const json::array_t& arr, const bool add_prefix)
    {
        if (add_prefix) out.push_back('[');

        bool prefix_required = true;
        if (use_type && !arr.empty())
        {
            const char first_prefix = prefix_of(arr.front());
            const bool same_prefix = std::all_of(arr.begin() + 1, arr.end(),
                                                 [this, first_prefix](const json& v)
            {
                return prefix_of(v) == first_prefix;
            });
            if (same_prefix && !(use_bjdata && bjdata_excluded(first_prefix)))
            {
                prefix_required = false;
                out.push_back('$');
                out.push_back(static_cast<uint8_t>(first_prefix));
            }
        }

        if (use_count)
        {
            out.push_back('#');
            ubjson_unsigned(static_cast<std::uint64_t>(arr.size()), true);
        }

        for (const auto& el : arr)
        {
            write_element(el, prefix_required);
        }

        if (!use_count)
        {
            out.push_back(']');
        }
    }

    void write_object(const json::object_t& obj, const bool add_prefix)
    {
        // BJData: JData ndarray detection (write_bjdata_ndarray) — on success
        // the object is replaced by the typed-array encoding
        if (use_bjdata && obj.size() == 3 &&
                obj.find("_ArrayType_") != obj.end() &&
                obj.find("_ArraySize_") != obj.end() &&
                obj.find("_ArrayData_") != obj.end() &&
                !write_bjdata_ndarray(obj))
        {
            return;
        }

        if (add_prefix) out.push_back('{');

        bool prefix_required = true;
        if (use_type && !obj.empty())
        {
            const char first_prefix = prefix_of(obj.begin()->second);
            const bool same_prefix = std::all_of(obj.begin(), obj.end(),
                                                 [this, first_prefix](const json::object_t::value_type& el)
            {
                return prefix_of(el.second) == first_prefix;
            });
            if (same_prefix && !(use_bjdata && bjdata_excluded(first_prefix)))
            {
                prefix_required = false;
                out.push_back('$');
                out.push_back(static_cast<uint8_t>(first_prefix));
            }
        }

        if (use_count)
        {
            out.push_back('#');
            ubjson_unsigned(static_cast<std::uint64_t>(obj.size()), true);
        }

        for (const auto& el : obj)
        {
            ubjson_unsigned(static_cast<std::uint64_t>(el.first.size()), true);
            out.insert(out.end(), el.first.begin(), el.first.end());
            write_element(el.second, prefix_required);
        }

        if (!use_count)
        {
            out.push_back('}');
        }
    }

    void write_binary(const json::binary_t& bin, const bool add_prefix)
    {
        if (add_prefix) out.push_back('[');

        // draft2 skips the '$' prefix for an EMPTY binary
        if (use_type && (bjdata_draft3 || !bin.empty()))
        {
            out.push_back('$');
            out.push_back(bjdata_draft3 ? 'B' : 'U');
        }

        if (use_count)
        {
            out.push_back('#');
            ubjson_unsigned(static_cast<std::uint64_t>(bin.size()), true);
        }

        if (use_type)
        {
            out.insert(out.end(), bin.begin(), bin.end());
        }
        else
        {
            for (const auto b : bin)
            {
                out.push_back(bjdata_draft3 ? 'B' : 'U');
                out.push_back(b);
            }
        }

        if (!use_count)
        {
            out.push_back(']');
        }
    }

    // markers excluded from the BJData '$' type optimization
    // (binary_writer.hpp bjdx list)
    static bool bjdata_excluded(const char prefix) noexcept
    {
        switch (prefix)
        {
            case '[': case '{': case 'S': case 'H':
            case 'T': case 'F': case 'N': case 'Z':
                return true;
            default:
                return false;
        }
    }

    // --- JData ndarray (binary_writer.hpp write_bjdata_ndarray); returns
    // true when the object is NOT an encodable ndarray (caller falls back to
    // a plain object), false after a successful typed-array encoding ---
    bool write_bjdata_ndarray(const json::object_t& obj)
    {
        static const std::map<std::string, char> kDtype =
        {
            {"uint8", 'U'}, {"int8", 'i'}, {"uint16", 'u'}, {"int16", 'I'},
            {"uint32", 'm'}, {"int32", 'l'}, {"uint64", 'M'}, {"int64", 'L'},
            {"single", 'd'}, {"double", 'D'}, {"char", 'C'}, {"byte", 'B'}
        };

        const auto t_it = kDtype.find(obj.at("_ArrayType_").template get<std::string>());
        if (t_it == kDtype.end())
        {
            return true; // unknown dtype -> plain object
        }
        const char dtype = t_it->second;

        // dimension product must be a valid non-negative length
        std::size_t len = (obj.at("_ArraySize_").empty() ? 0 : 1);
        for (const auto& el : obj.at("_ArraySize_"))
        {
            if (!el.is_number_integer() || (!el.is_number_unsigned() && el.template get<std::int64_t>() < 0))
            {
                return true;
            }
            const auto dim = el.template get<std::uint64_t>();
            if (dim > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
            {
                return true; // a dimension that does not fit std::size_t
            }
            const auto dim_size = static_cast<std::size_t>(dim);
            if (dim_size != 0 && len > (std::numeric_limits<std::size_t>::max)() / dim_size)
            {
                return true;
            }
            len *= dim_size;
        }

        if (obj.at("_ArrayData_").size() != len)
        {
            return true;
        }

        const bool ndarray_is_float = (dtype == 'd' || dtype == 'D');
        for (const auto& el : obj.at("_ArrayData_"))
        {
            if (ndarray_is_float ? !el.is_number_float() : !el.is_number_integer())
            {
                return true;
            }
        }

        out.push_back('[');
        out.push_back('$');
        out.push_back(static_cast<uint8_t>(dtype));
        out.push_back('#');

        write_element(obj.at("_ArraySize_"), true); // a full optimized array

        // compact element payloads (write_number(..., OutputIsLittleEndian=true))
        const auto& data = obj.at("_ArrayData_");
        switch (dtype)
        {
            case 'U': case 'C': case 'B':
                for (const auto& el : data) write_num(static_cast<std::uint8_t>(el.template get<std::uint64_t>()), true);
                break;
            case 'i':
                for (const auto& el : data) write_num(static_cast<std::int8_t>(el.template get<std::int64_t>()), true);
                break;
            case 'u':
                for (const auto& el : data) write_num(static_cast<std::uint16_t>(el.template get<std::uint64_t>()), true);
                break;
            case 'I':
                for (const auto& el : data) write_num(static_cast<std::int16_t>(el.template get<std::int64_t>()), true);
                break;
            case 'm':
                for (const auto& el : data) write_num(static_cast<std::uint32_t>(el.template get<std::uint64_t>()), true);
                break;
            case 'l':
                for (const auto& el : data) write_num(static_cast<std::int32_t>(el.template get<std::int64_t>()), true);
                break;
            case 'M':
                for (const auto& el : data) write_num(el.template get<std::uint64_t>(), true);
                break;
            case 'L':
                for (const auto& el : data) write_num(el.template get<std::int64_t>(), true);
                break;
            case 'd':
                for (const auto& el : data) write_num(static_cast<float>(el.template get<double>()), true);
                break;
            case 'D':
                for (const auto& el : data) write_num(el.template get<double>(), true);
                break;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven CBOR reader (M4B-3).
//
// Mirror image of reflection_cbor_serializer (and of the library's
// from_cbor): value routing comes from the reflection-generated kCborLoads
// table (first byte -> {union slot, payload kind}); definite/indefinite
// containers, tag handling (error/ignore/store, aligned with
// cbor_tag_handler_t), half-precision floats and binary subtypes are plain
// mechanics on top of the table. Byte-level behavior is differential-tested
// against from_cbor (m4f_binary_readers.cpp).
// ---------------------------------------------------------------------------
struct reflection_cbor_parser
{
    std::string err;

    explicit reflection_cbor_parser(
        const std::vector<std::uint8_t>& in,
        const nlohmann::detail::cbor_tag_handler_t th = nlohmann::detail::cbor_tag_handler_t::error)
        : data(in),
          tag_handler(th)
    {}

    bool parse(basic_json_reflection& out)
    {
        pos = 0;
        json top;
        if (!parse_cbor_value(top))
        {
            return false;
        }
        out.assign_from(top);
        return true;
    }

    const std::string& error() const
    {
        return err;
    }

  private:
    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;
    const nlohmann::detail::cbor_tag_handler_t tag_handler;

    bool fail(const char* what)
    {
        if (err.empty())
        {
            err = what;
        }
        return false;
    }

    bool at_end() const
    {
        return pos >= data.size();
    }

    bool take(std::uint8_t& b)
    {
        if (at_end())
        {
            return fail("unexpected end of input");
        }
        b = data[pos++];
        return true;
    }

    // big-endian fixed-width read (CBOR is network order)
    template<typename T>
    bool read_be(T& out)
    {
        if (data.size() - pos < sizeof(T))
        {
            return fail("truncated number");
        }
        T v{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            v |= static_cast<T>(data[pos + i]) << (8 * (sizeof(T) - 1 - i));
        }
        pos += sizeof(T);
        out = v;
        return true;
    }

    bool take_bytes(const std::size_t n, std::string& out)
    {
        if (data.size() - pos < n)
        {
            return fail("truncated payload");
        }
        out.assign(reinterpret_cast<const char*>(data.data()) + pos, n);
        pos += n;
        return true;
    }

    // RFC 8949 Appendix D half-precision decode (same as the library)
    static double half_to_double(const std::uint16_t half)
    {
        const int exp = (half >> 10u) & 0x1Fu;
        const unsigned int mant = half & 0x3FFu;
        double val = 0.0;
        switch (exp)
        {
            case 0:
                val = std::ldexp(static_cast<double>(mant), -24);
                break;
            case 31:
                val = (mant == 0)
                      ? std::numeric_limits<double>::infinity()
                      : std::numeric_limits<double>::quiet_NaN();
                break;
            default:
                val = std::ldexp(static_cast<double>(mant + 1024), exp - 25);
                break;
        }
        return (half & 0x8000u) != 0 ? -val : val;
    }

    bool read_half(double& out)
    {
        std::uint16_t half{};
        if (!read_be(half))
        {
            return false;
        }
        out = half_to_double(half);
        return true;
    }

    // read a definite-length payload (bytes or text); `code` is the already
    // consumed first byte, whose kind selects the length width
    template<bool IsText>
    bool read_payload(const std::uint8_t code, std::string& out)
    {
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
        std::uint64_t len = 0;
        switch (kind)
        {
            case refl_detail::cbor_payload_kind::bytes_immediate:
            case refl_detail::cbor_payload_kind::text_immediate:
                len = code & 0x1Fu;
                break;
            case refl_detail::cbor_payload_kind::bytes8:
            case refl_detail::cbor_payload_kind::text8:
            {
                std::uint8_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                break;
            }
            case refl_detail::cbor_payload_kind::bytes16:
            case refl_detail::cbor_payload_kind::text16:
            {
                std::uint16_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                break;
            }
            case refl_detail::cbor_payload_kind::bytes32:
            case refl_detail::cbor_payload_kind::text32:
            {
                std::uint32_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                break;
            }
            case refl_detail::cbor_payload_kind::bytes64:
            case refl_detail::cbor_payload_kind::text64:
                if (!read_be(len))
                {
                    return false;
                }
                break;
            default:
                return fail("unexpected payload header");
        }
        if (len > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return fail("excessive length");
        }
        return take_bytes(static_cast<std::size_t>(len), out);
    }

    // indefinite-length text: chunks of definite text until break (0xFF)
    bool read_text_indefinite(std::string& out)
    {
        while (true)
        {
            std::uint8_t b{};
            if (!take(b))
            {
                return fail("unterminated indefinite string");
            }
            if (b == 0xFF)
            {
                break;
            }
            const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[b].kind);
            if (kind == refl_detail::cbor_payload_kind::text_indefinite)
            {
                std::string chunk;
                if (!read_text_indefinite(chunk))
                {
                    return false;
                }
                out += chunk;
                continue;
            }
            if (!(kind >= refl_detail::cbor_payload_kind::text_immediate &&
                  kind <= refl_detail::cbor_payload_kind::text64))
            {
                return fail("invalid chunk in indefinite text");
            }
            std::string chunk;
            if (!read_payload<true>(b, chunk))
            {
                return false;
            }
            out += chunk;
        }
        return true;
    }

    // indefinite-length bytes: chunks of definite bytes until break (0xFF)
    bool read_binary_indefinite(json::binary_t& out)
    {
        while (true)
        {
            std::uint8_t b{};
            if (!take(b))
            {
                return fail("unterminated indefinite binary");
            }
            if (b == 0xFF)
            {
                break;
            }
            const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[b].kind);
            if (kind == refl_detail::cbor_payload_kind::bytes_indefinite)
            {
                json::binary_t chunk;
                if (!read_binary_indefinite(chunk))
                {
                    return false;
                }
                out.insert(out.end(), chunk.begin(), chunk.end());
                continue;
            }
            if (!(kind >= refl_detail::cbor_payload_kind::bytes_immediate &&
                  kind <= refl_detail::cbor_payload_kind::bytes64))
            {
                return fail("invalid chunk in indefinite binary");
            }
            std::string chunk;
            if (!read_payload<false>(b, chunk))
            {
                return false;
            }
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return true;
    }

    // --- main dispatch: `code` is the already-consumed first byte ---
    bool parse_cbor_one(const std::uint8_t code, json& out)
    {
        const refl_detail::cbor_load_entry entry = kCborLoads[code];
        switch (static_cast<refl_detail::cbor_payload_kind>(entry.kind))
        {
            case refl_detail::cbor_payload_kind::uint_immediate:
                out = static_cast<json::number_unsigned_t>(code);
                return true;
            case refl_detail::cbor_payload_kind::uint8:
            {
                std::uint8_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::cbor_payload_kind::uint16:
            {
                std::uint16_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::cbor_payload_kind::uint32:
            {
                std::uint32_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::cbor_payload_kind::uint64:
            {
                std::uint64_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::cbor_payload_kind::negint_immediate:
                out = static_cast<json::number_integer_t>(-1) - static_cast<json::number_integer_t>(code & 0x1Fu);
                return true;
            case refl_detail::cbor_payload_kind::negint8:
            case refl_detail::cbor_payload_kind::negint16:
            case refl_detail::cbor_payload_kind::negint32:
            case refl_detail::cbor_payload_kind::negint64:
                return parse_cbor_negative(code, out);
            case refl_detail::cbor_payload_kind::bytes_immediate:
            case refl_detail::cbor_payload_kind::bytes8:
            case refl_detail::cbor_payload_kind::bytes16:
            case refl_detail::cbor_payload_kind::bytes32:
            case refl_detail::cbor_payload_kind::bytes64:
            {
                std::string raw;
                if (!read_payload<false>(code, raw))
                {
                    return false;
                }
                out = json::binary_t(std::vector<std::uint8_t>(raw.begin(), raw.end()));
                return true;
            }
            case refl_detail::cbor_payload_kind::bytes_indefinite:
            {
                json::binary_t bin;
                if (!read_binary_indefinite(bin))
                {
                    return false;
                }
                out = std::move(bin);
                return true;
            }
            case refl_detail::cbor_payload_kind::text_immediate:
            case refl_detail::cbor_payload_kind::text8:
            case refl_detail::cbor_payload_kind::text16:
            case refl_detail::cbor_payload_kind::text32:
            case refl_detail::cbor_payload_kind::text64:
            {
                std::string s;
                if (!read_payload<true>(code, s))
                {
                    return false;
                }
                out = std::move(s);
                return true;
            }
            case refl_detail::cbor_payload_kind::text_indefinite:
            {
                std::string s;
                if (!read_text_indefinite(s))
                {
                    return false;
                }
                out = std::move(s);
                return true;
            }
            case refl_detail::cbor_payload_kind::array_immediate:
            case refl_detail::cbor_payload_kind::array8:
            case refl_detail::cbor_payload_kind::array16:
            case refl_detail::cbor_payload_kind::array32:
            case refl_detail::cbor_payload_kind::array64:
                return parse_cbor_array(code, out);
            case refl_detail::cbor_payload_kind::array_indefinite:
                return parse_cbor_array_indefinite(out);
            case refl_detail::cbor_payload_kind::map_immediate:
            case refl_detail::cbor_payload_kind::map8:
            case refl_detail::cbor_payload_kind::map16:
            case refl_detail::cbor_payload_kind::map32:
            case refl_detail::cbor_payload_kind::map64:
                return parse_cbor_object(code, out);
            case refl_detail::cbor_payload_kind::map_indefinite:
                return parse_cbor_object_indefinite(out);
            case refl_detail::cbor_payload_kind::tag_noarg:
            case refl_detail::cbor_payload_kind::tag_1byte:
            case refl_detail::cbor_payload_kind::tag_2byte:
            case refl_detail::cbor_payload_kind::tag_4byte:
            case refl_detail::cbor_payload_kind::tag_8byte:
                return parse_cbor_tag(code, out);
            case refl_detail::cbor_payload_kind::boolean_false:
                out = false;
                return true;
            case refl_detail::cbor_payload_kind::boolean_true:
                out = true;
                return true;
            case refl_detail::cbor_payload_kind::null_fixed:
                out = nullptr;
                return true;
            case refl_detail::cbor_payload_kind::half_float:
            {
                double v{};
                return read_half(v) && (out = v, true);
            }
            case refl_detail::cbor_payload_kind::float32:
            {
                std::uint32_t bits{};
                if (!read_be(bits))
                {
                    return false;
                }
                float v{};
                std::memcpy(&v, &bits, sizeof(float));
                out = v;
                return true;
            }
            case refl_detail::cbor_payload_kind::float64:
            {
                std::uint64_t bits{};
                if (!read_be(bits))
                {
                    return false;
                }
                double v{};
                std::memcpy(&v, &bits, sizeof(double));
                out = v;
                return true;
            }
            default:
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", code);
                err = std::string("invalid CBOR byte ") + hex;
                return false;
            }
        }
    }

    // negative integer: -1 - n, rejecting n > INT64_MAX (library error 112)
    bool parse_cbor_negative(const std::uint8_t code, json& out)
    {
        std::uint64_t n = 0;
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
        switch (kind)
        {
            case refl_detail::cbor_payload_kind::negint8:
            {
                std::uint8_t v{};
                if (!read_be(v))
                {
                    return false;
                }
                n = v;
                break;
            }
            case refl_detail::cbor_payload_kind::negint16:
            {
                std::uint16_t v{};
                if (!read_be(v))
                {
                    return false;
                }
                n = v;
                break;
            }
            case refl_detail::cbor_payload_kind::negint32:
            {
                std::uint32_t v{};
                if (!read_be(v))
                {
                    return false;
                }
                n = v;
                break;
            }
            default:
                if (!read_be(n))
                {
                    return false;
                }
                break;
        }
        const auto max_val = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        if (n > max_val)
        {
            return fail("negative integer overflow");
        }
        out = static_cast<json::number_integer_t>(-1) - static_cast<json::number_integer_t>(n);
        return true;
    }

    // definite-length array; `code` selects the count width
    bool parse_cbor_array(const std::uint8_t code, json& out)
    {
        std::uint64_t count = 0;
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
        switch (kind)
        {
            case refl_detail::cbor_payload_kind::array_immediate:
                count = code & 0x1Fu;
                break;
            case refl_detail::cbor_payload_kind::array8:
            {
                std::uint8_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            case refl_detail::cbor_payload_kind::array16:
            {
                std::uint16_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            case refl_detail::cbor_payload_kind::array32:
            {
                std::uint32_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            default:
                if (!read_be(count))
                {
                    return false;
                }
                break;
        }
        if (count >= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return fail("excessive array size");
        }
        json::array_t arr;
        arr.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i)
        {
            json el;
            if (!parse_cbor_value(el))
            {
                return false;
            }
            arr.push_back(std::move(el));
        }
        out = std::move(arr);
        return true;
    }

    bool parse_cbor_array_indefinite(json& out)
    {
        json::array_t arr;
        while (true)
        {
            std::uint8_t b{};
            if (!take(b))
            {
                return fail("unterminated indefinite array");
            }
            if (b == 0xFF)
            {
                break;
            }
            json el;
            if (!parse_cbor_one(b, el))
            {
                return false;
            }
            arr.push_back(std::move(el));
        }
        out = std::move(arr);
        return true;
    }

    // definite-length object; keys must be text strings (library contract)
    bool parse_cbor_object(const std::uint8_t code, json& out)
    {
        std::uint64_t count = 0;
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
        switch (kind)
        {
            case refl_detail::cbor_payload_kind::map_immediate:
                count = code & 0x1Fu;
                break;
            case refl_detail::cbor_payload_kind::map8:
            {
                std::uint8_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            case refl_detail::cbor_payload_kind::map16:
            {
                std::uint16_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            case refl_detail::cbor_payload_kind::map32:
            {
                std::uint32_t c{};
                if (!read_be(c))
                {
                    return false;
                }
                count = c;
                break;
            }
            default:
                if (!read_be(count))
                {
                    return false;
                }
                break;
        }
        if (count >= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return fail("excessive map size");
        }
        json::object_t obj;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            std::string key;
            if (!read_cbor_key(key))
            {
                return false;
            }
            json val;
            if (!parse_cbor_value(val))
            {
                return false;
            }
            obj[std::move(key)] = std::move(val);
        }
        out = std::move(obj);
        return true;
    }

    bool parse_cbor_object_indefinite(json& out)
    {
        json::object_t obj;
        while (true)
        {
            std::uint8_t b{};
            if (!take(b))
            {
                return fail("unterminated indefinite map");
            }
            if (b == 0xFF)
            {
                break;
            }
            // b is the first byte of the key; classify it directly
            std::string key;
            if (!parse_cbor_key_from(b, key))
            {
                return false;
            }
            json val;
            if (!parse_cbor_value(val))
            {
                return false;
            }
            obj[std::move(key)] = std::move(val);
        }
        out = std::move(obj);
        return true;
    }

    // read a CBOR map key: consume the first byte, require a text type
    bool read_cbor_key(std::string& key)
    {
        std::uint8_t b{};
        if (!take(b))
        {
            return false;
        }
        return parse_cbor_key_from(b, key);
    }

    bool parse_cbor_key_from(const std::uint8_t b, std::string& key)
    {
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[b].kind);
        if (kind >= refl_detail::cbor_payload_kind::text_immediate &&
                kind <= refl_detail::cbor_payload_kind::text64)
        {
            return read_payload<true>(b, key);
        }
        if (kind == refl_detail::cbor_payload_kind::text_indefinite)
        {
            return read_text_indefinite(key);
        }
        return fail("CBOR map key must be a text string");
    }

    // tag handling (aligned with cbor_tag_handler_t): error -> reject;
    // ignore -> skip the tag value (if any) and parse the next item;
    // store -> use the tag value as a binary subtype (0xD8-0xDB) and parse
    // the following binary item
    bool parse_cbor_tag(const std::uint8_t code, json& out)
    {
        switch (tag_handler)
        {
            case nlohmann::detail::cbor_tag_handler_t::error:
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", code);
                err = std::string("tagged CBOR item not allowed: ") + hex;
                return false;
            }
            case nlohmann::detail::cbor_tag_handler_t::ignore:
            {
                if (!skip_tag_value(code))
                {
                    return false;
                }
                return parse_cbor_value(out);
            }
            case nlohmann::detail::cbor_tag_handler_t::store:
            {
                if (!skip_tag_value(code))
                {
                    return false;
                }
                // only 0xD8-0xDB carry a subtype; others are ignored
                const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
                if (kind == refl_detail::cbor_payload_kind::tag_noarg)
                {
                    return parse_cbor_value(out);
                }
                // the tagged item must be a byte string
                std::uint8_t next{};
                if (!take(next))
                {
                    return fail("missing byte string after subtype tag");
                }
                const auto nk = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[next].kind);
                if (nk == refl_detail::cbor_payload_kind::bytes_indefinite)
                {
                    json::binary_t bin;
                    if (!read_binary_indefinite(bin))
                    {
                        return false;
                    }
                    bin.set_subtype(tag_subtype);
                    out = std::move(bin);
                    return true;
                }
                if (!(nk >= refl_detail::cbor_payload_kind::bytes_immediate &&
                        nk <= refl_detail::cbor_payload_kind::bytes64))
                {
                    return fail("tagged item must be a byte string");
                }
                std::string raw;
                if (!read_payload<false>(next, raw))
                {
                    return false;
                }
                json::binary_t bin(std::vector<std::uint8_t>(raw.begin(), raw.end()));
                bin.set_subtype(tag_subtype);
                out = std::move(bin);
                return true;
            }
        }
        return fail("unreachable tag handler");
    }

    // consume the tag payload (if 0xD8-0xDB) into tag_subtype
    bool skip_tag_value(const std::uint8_t code)
    {
        const auto kind = static_cast<refl_detail::cbor_payload_kind>(kCborLoads[code].kind);
        switch (kind)
        {
            case refl_detail::cbor_payload_kind::tag_noarg:
                return true;
            case refl_detail::cbor_payload_kind::tag_1byte:
            {
                std::uint8_t v{};
                return read_be(v) && (tag_subtype = v, true);
            }
            case refl_detail::cbor_payload_kind::tag_2byte:
            {
                std::uint16_t v{};
                return read_be(v) && (tag_subtype = v, true);
            }
            case refl_detail::cbor_payload_kind::tag_4byte:
            {
                std::uint32_t v{};
                return read_be(v) && (tag_subtype = v, true);
            }
            case refl_detail::cbor_payload_kind::tag_8byte:
            {
                std::uint64_t v{};
                return read_be(v) && (tag_subtype = v, true);
            }
            default:
                return fail("invalid tag byte");
        }
    }

    bool parse_cbor_value(json& out)
    {
        std::uint8_t b{};
        if (!take(b))
        {
            return false;
        }
        return parse_cbor_one(b, out);
    }

    json::binary_t::subtype_type tag_subtype = 0;
};

// ---------------------------------------------------------------------------
// Reflection-driven MessagePack reader (M4B-3).
//
// Mirror image of reflection_msgpack_serializer: value routing comes from the
// reflection-generated kMsgpackLoads table (first byte -> {union slot,
// payload kind}); range-dependent forms (fixint/fixmap/fixarray/fixstr,
// bin/ext/fixext widths, str/array/map 8/16/32) are decoded inside the
// per-kind actions. Byte-level behavior is differential-tested against
// from_msgpack (m4f_binary_readers.cpp).
// ---------------------------------------------------------------------------
struct reflection_msgpack_parser
{
    std::string err;

    explicit reflection_msgpack_parser(const std::vector<std::uint8_t>& in)
        : data(in)
    {}

    bool parse(basic_json_reflection& out)
    {
        pos = 0;
        json top;
        if (!parse_value(top))
        {
            return false;
        }
        out.assign_from(top);
        return true;
    }

    const std::string& error() const
    {
        return err;
    }

  private:
    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;

    bool fail(const char* what)
    {
        if (err.empty())
        {
            err = what;
        }
        return false;
    }

    bool at_end() const
    {
        return pos >= data.size();
    }

    bool take(std::uint8_t& b)
    {
        if (at_end())
        {
            return fail("unexpected end of input");
        }
        b = data[pos++];
        return true;
    }

    template<typename T>
    bool read_be(T& out)
    {
        if (data.size() - pos < sizeof(T))
        {
            return fail("truncated number");
        }
        T v{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            v |= static_cast<T>(data[pos + i]) << (8 * (sizeof(T) - 1 - i));
        }
        pos += sizeof(T);
        out = v;
        return true;
    }

    bool take_bytes(const std::size_t n, std::string& out)
    {
        if (data.size() - pos < n)
        {
            return fail("truncated payload");
        }
        out.assign(reinterpret_cast<const char*>(data.data()) + pos, n);
        pos += n;
        return true;
    }

    // string length from an already-consumed first byte
    bool string_len(const std::uint8_t code, std::size_t& len)
    {
        const auto kind = static_cast<refl_detail::msgpack_payload_kind>(kMsgpackLoads[code].kind);
        switch (kind)
        {
            case refl_detail::msgpack_payload_kind::fixstr:
                len = code & 0x1Fu;
                return true;
            case refl_detail::msgpack_payload_kind::str8:
            {
                std::uint8_t l{};
                return read_be(l) && (len = l, true);
            }
            case refl_detail::msgpack_payload_kind::str16:
            {
                std::uint16_t l{};
                return read_be(l) && (len = l, true);
            }
            case refl_detail::msgpack_payload_kind::str32:
            {
                std::uint32_t l{};
                return read_be(l) && (len = l, true);
            }
            default:
                return fail("expected a MessagePack string header");
        }
    }

    bool read_string(const std::uint8_t code, std::string& out)
    {
        std::size_t len = 0;
        if (!string_len(code, len))
        {
            return false;
        }
        return take_bytes(len, out);
    }

    // binary (bin*/ext*/fixext*): length (and subtype for ext/fixext)
    bool read_binary(const std::uint8_t code, json::binary_t& out)
    {
        const auto kind = static_cast<refl_detail::msgpack_payload_kind>(kMsgpackLoads[code].kind);
        std::size_t len = 0;
        bool is_ext = false;
        switch (kind)
        {
            case refl_detail::msgpack_payload_kind::bin8:
            {
                std::uint8_t l{};
                return read_be(l) && take_bytes(l, out_buf) && (out.assign(out_buf.begin(), out_buf.end()), true);
            }
            case refl_detail::msgpack_payload_kind::bin16:
            {
                std::uint16_t l{};
                return read_be(l) && take_bytes(l, out_buf) && (out.assign(out_buf.begin(), out_buf.end()), true);
            }
            case refl_detail::msgpack_payload_kind::bin32:
            {
                std::uint32_t l{};
                return read_be(l) && take_bytes(l, out_buf) && (out.assign(out_buf.begin(), out_buf.end()), true);
            }
            case refl_detail::msgpack_payload_kind::ext8:
            {
                std::uint8_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                is_ext = true;
                break;
            }
            case refl_detail::msgpack_payload_kind::ext16:
            {
                std::uint16_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                is_ext = true;
                break;
            }
            case refl_detail::msgpack_payload_kind::ext32:
            {
                std::uint32_t l{};
                if (!read_be(l))
                {
                    return false;
                }
                len = l;
                is_ext = true;
                break;
            }
            case refl_detail::msgpack_payload_kind::fixext1:
                len = 1;
                is_ext = true;
                break;
            case refl_detail::msgpack_payload_kind::fixext2:
                len = 2;
                is_ext = true;
                break;
            case refl_detail::msgpack_payload_kind::fixext4:
                len = 4;
                is_ext = true;
                break;
            case refl_detail::msgpack_payload_kind::fixext8:
                len = 8;
                is_ext = true;
                break;
            case refl_detail::msgpack_payload_kind::fixext16:
                len = 16;
                is_ext = true;
                break;
            default:
                return fail("expected a MessagePack binary header");
        }
        if (is_ext)
        {
            std::uint8_t subtype{};
            if (!take(subtype))
            {
                return false;
            }
            out.set_subtype(subtype);
        }
        if (!take_bytes(len, out_buf))
        {
            return false;
        }
        out.assign(out_buf.begin(), out_buf.end());
        return true;
    }

    std::string out_buf; // reused scratch for binary payloads

    // array / map counts from an already-consumed first byte
    bool container_count(const std::uint8_t code, std::size_t& count)
    {
        const auto kind = static_cast<refl_detail::msgpack_payload_kind>(kMsgpackLoads[code].kind);
        switch (kind)
        {
            case refl_detail::msgpack_payload_kind::fixarray:
                count = code & 0x0Fu;
                return true;
            case refl_detail::msgpack_payload_kind::fixmap:
                count = code & 0x0Fu;
                return true;
            case refl_detail::msgpack_payload_kind::array16:
            {
                std::uint16_t c{};
                return read_be(c) && (count = c, true);
            }
            case refl_detail::msgpack_payload_kind::array32:
            {
                std::uint32_t c{};
                return read_be(c) && (count = c, true);
            }
            case refl_detail::msgpack_payload_kind::map16:
            {
                std::uint16_t c{};
                return read_be(c) && (count = c, true);
            }
            case refl_detail::msgpack_payload_kind::map32:
            {
                std::uint32_t c{};
                return read_be(c) && (count = c, true);
            }
            default:
                return fail("expected a MessagePack container header");
        }
    }

    // --- main dispatch: `code` is the already-consumed first byte ---
    bool parse_one(const std::uint8_t code, json& out)
    {
        const refl_detail::msgpack_load_entry entry = kMsgpackLoads[code];
        switch (static_cast<refl_detail::msgpack_payload_kind>(entry.kind))
        {
            case refl_detail::msgpack_payload_kind::positive_fixint:
                out = static_cast<json::number_unsigned_t>(code);
                return true;
            case refl_detail::msgpack_payload_kind::negative_fixint:
                out = static_cast<json::number_integer_t>(static_cast<std::int8_t>(code));
                return true;
            case refl_detail::msgpack_payload_kind::nil_fixed:
                out = nullptr;
                return true;
            case refl_detail::msgpack_payload_kind::boolean_false:
                out = false;
                return true;
            case refl_detail::msgpack_payload_kind::boolean_true:
                out = true;
                return true;
            case refl_detail::msgpack_payload_kind::fixstr:
            case refl_detail::msgpack_payload_kind::str8:
            case refl_detail::msgpack_payload_kind::str16:
            case refl_detail::msgpack_payload_kind::str32:
            {
                std::string s;
                if (!read_string(code, s))
                {
                    return false;
                }
                out = std::move(s);
                return true;
            }
            case refl_detail::msgpack_payload_kind::bin8:
            case refl_detail::msgpack_payload_kind::bin16:
            case refl_detail::msgpack_payload_kind::bin32:
            case refl_detail::msgpack_payload_kind::ext8:
            case refl_detail::msgpack_payload_kind::ext16:
            case refl_detail::msgpack_payload_kind::ext32:
            case refl_detail::msgpack_payload_kind::fixext1:
            case refl_detail::msgpack_payload_kind::fixext2:
            case refl_detail::msgpack_payload_kind::fixext4:
            case refl_detail::msgpack_payload_kind::fixext8:
            case refl_detail::msgpack_payload_kind::fixext16:
            {
                json::binary_t bin;
                if (!read_binary(code, bin))
                {
                    return false;
                }
                out = std::move(bin);
                return true;
            }
            case refl_detail::msgpack_payload_kind::float32:
            {
                std::uint32_t bits{};
                if (!read_be(bits))
                {
                    return false;
                }
                float v{};
                std::memcpy(&v, &bits, sizeof(float));
                out = v;
                return true;
            }
            case refl_detail::msgpack_payload_kind::float64:
            {
                std::uint64_t bits{};
                if (!read_be(bits))
                {
                    return false;
                }
                double v{};
                std::memcpy(&v, &bits, sizeof(double));
                out = v;
                return true;
            }
            case refl_detail::msgpack_payload_kind::uint8:
            {
                std::uint8_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::uint16:
            {
                std::uint16_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::uint32:
            {
                std::uint32_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::uint64:
            {
                std::uint64_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::int8:
            {
                std::int8_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::int16:
            {
                std::int16_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::int32:
            {
                std::int32_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::int64:
            {
                std::int64_t v{};
                return read_be(v) && (out = v, true);
            }
            case refl_detail::msgpack_payload_kind::fixarray:
            case refl_detail::msgpack_payload_kind::array16:
            case refl_detail::msgpack_payload_kind::array32:
            {
                std::size_t count = 0;
                if (!container_count(code, count))
                {
                    return false;
                }
                json::array_t arr;
                arr.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    json el;
                    if (!parse_value(el))
                    {
                        return false;
                    }
                    arr.push_back(std::move(el));
                }
                out = std::move(arr);
                return true;
            }
            case refl_detail::msgpack_payload_kind::fixmap:
            case refl_detail::msgpack_payload_kind::map16:
            case refl_detail::msgpack_payload_kind::map32:
            {
                std::size_t count = 0;
                if (!container_count(code, count))
                {
                    return false;
                }
                json::object_t obj;
                for (std::size_t i = 0; i < count; ++i)
                {
                    std::string key;
                    if (!read_map_key(key))
                    {
                        return false;
                    }
                    json val;
                    if (!parse_value(val))
                    {
                        return false;
                    }
                    obj[std::move(key)] = std::move(val);
                }
                out = std::move(obj);
                return true;
            }
            default:
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", code);
                err = std::string("invalid MessagePack byte ") + hex;
                return false;
            }
        }
    }

    // MessagePack map keys are strings (fixstr/str8/16/32)
    bool read_map_key(std::string& key)
    {
        std::uint8_t b{};
        if (!take(b))
        {
            return false;
        }
        const auto kind = static_cast<refl_detail::msgpack_payload_kind>(kMsgpackLoads[b].kind);
        if (kind != refl_detail::msgpack_payload_kind::fixstr &&
                kind != refl_detail::msgpack_payload_kind::str8 &&
                kind != refl_detail::msgpack_payload_kind::str16 &&
                kind != refl_detail::msgpack_payload_kind::str32)
        {
            return fail("MessagePack map key must be a string");
        }
        return read_string(b, key);
    }

    bool parse_value(json& out)
    {
        std::uint8_t b{};
        if (!take(b))
        {
            return false;
        }
        return parse_one(b, out);
    }
};

// ---------------------------------------------------------------------------
// Reflection-driven UBJSON / BJData reader (M4B-3).
//
// Mirror image of reflection_ubjson_optimized_serializer (and of the
// library's from_ubjson / from_bjdata): value routing comes from the
// reflection-generated kUbjsonLoads table (marker byte -> {union slot,
// payload kind}); the optimized container modes ('$' type / '#' count
// prefixes), the BJData dialect (little-endian numbers, 'u'/'m'/'M' width
// rungs, 'h' half-float, 'B' byte / binary, JData ndarray decoding) and the
// high-precision number form ('H') are plain mechanics on top of the table.
// `use_bjdata` switches the dialect exactly like the library's
// input_format_t::{ubjson,bjdata}. Byte-level behavior is
// differential-tested against from_ubjson / from_bjdata
// (m4f_binary_readers.cpp).
// ---------------------------------------------------------------------------
struct reflection_ubjson_parser
{
    std::string err;

    explicit reflection_ubjson_parser(const std::vector<std::uint8_t>& in,
                                      const bool use_bjdata_ = false)
        : data(in),
          use_bjdata(use_bjdata_)
    {}

    bool parse(basic_json_reflection& out)
    {
        pos = 0;
        ndarray_pending = false;
        ndarray_dims.clear();
        ndarray_type = 0;
        json top;
        if (!parse_value(top))
        {
            return false;
        }
        out.assign_from(top);
        return true;
    }

    const std::string& error() const
    {
        return err;
    }

  private:
    static constexpr std::size_t npos = (std::numeric_limits<std::size_t>::max)();

    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;
    const bool use_bjdata;

    bool fail(const char* what)
    {
        if (err.empty())
        {
            err = what;
        }
        return false;
    }

    bool at_end() const
    {
        return pos >= data.size();
    }

    bool take(std::uint8_t& b)
    {
        if (at_end())
        {
            return fail("unexpected end of input");
        }
        b = data[pos++];
        return true;
    }

    // skip no-op markers ('N') — the library's get_ignore_noop
    bool take_ignore_noop(std::uint8_t& b)
    {
        do
        {
            if (!take(b))
            {
                return false;
            }
        }
        while (b == 'N');
        return true;
    }

    // push back the last byte (used when a container prologue is absent and
    // the byte belongs to the first element / key instead)
    void unget()
    {
        if (pos > 0)
        {
            --pos;
        }
    }

    template<typename T>
    bool read_number(T& out)
    {
        if (data.size() - pos < sizeof(T))
        {
            return fail("truncated number");
        }
        T v{};
        if (use_bjdata)
        {
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                v |= static_cast<T>(data[pos + i]) << (8 * i); // little-endian
            }
        }
        else
        {
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                v |= static_cast<T>(data[pos + i]) << (8 * (sizeof(T) - 1 - i)); // big-endian
            }
        }
        pos += sizeof(T);
        out = v;
        return true;
    }

    bool take_bytes(const std::size_t n, std::string& out)
    {
        if (data.size() - pos < n)
        {
            return fail("truncated payload");
        }
        out.assign(reinterpret_cast<const char*>(data.data()) + pos, n);
        pos += n;
        return true;
    }

    // RFC 8949 half decode — the BJData 'h' marker is little-endian, so the
    // byte order differs from CBOR's big-endian half (handled per call site)
    static double half_to_double(const std::uint16_t half)
    {
        const int exp = (half >> 10u) & 0x1Fu;
        const unsigned int mant = half & 0x3FFu;
        double val = 0.0;
        switch (exp)
        {
            case 0:
                val = std::ldexp(static_cast<double>(mant), -24);
                break;
            case 31:
                val = (mant == 0)
                      ? std::numeric_limits<double>::infinity()
                      : std::numeric_limits<double>::quiet_NaN();
                break;
            default:
                val = std::ldexp(static_cast<double>(mant + 1024), exp - 25);
                break;
        }
        return (half & 0x8000u) != 0 ? -val : val;
    }

    // 'h' half-float: two bytes, little-endian in BJData (byte1 is low)
    bool read_half(double& out)
    {
        std::uint8_t byte1{};
        std::uint8_t byte2{};
        if (!take(byte1) || !take(byte2))
        {
            return false;
        }
        const auto half = static_cast<std::uint16_t>((static_cast<std::uint16_t>(byte2) << 8u) | byte1);
        out = half_to_double(half);
        return true;
    }

    // --- length / size value after '#' (get_ubjson_size_value) ---
    // reads a single size marker + its number; `prefix` may be 0 to read one
    bool read_size_value(std::size_t& result, std::uint8_t prefix = 0)
    {
        constexpr auto max_size = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());

        if (prefix == 0 && !take_ignore_noop(prefix))
        {
            return false;
        }
        switch (prefix)
        {
            case 'U':
            {
                std::uint8_t n{};
                return read_number(n) && (result = n, true);
            }
            case 'i':
            {
                std::int8_t n{};
                if (!read_number(n))
                {
                    return false;
                }
                if (n < 0)
                {
                    return fail("count in an optimized container must be positive");
                }
                result = static_cast<std::size_t>(n);
                return true;
            }
            case 'I':
            {
                std::int16_t n{};
                if (!read_number(n))
                {
                    return false;
                }
                if (n < 0)
                {
                    return fail("count in an optimized container must be positive");
                }
                result = static_cast<std::size_t>(n);
                return true;
            }
            case 'l':
            {
                std::int32_t n{};
                if (!read_number(n))
                {
                    return false;
                }
                if (n < 0)
                {
                    return fail("count in an optimized container must be positive");
                }
                result = static_cast<std::size_t>(n);
                return true;
            }
            case 'L':
            {
                std::int64_t n{};
                if (!read_number(n))
                {
                    return false;
                }
                if (n < 0)
                {
                    return fail("count in an optimized container must be positive");
                }
                if (static_cast<std::uint64_t>(n) > max_size)
                {
                    return fail("integer value overflow");
                }
                result = static_cast<std::size_t>(n);
                return true;
            }
            case 'u':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint16_t n{};
                return read_number(n) && (result = n, true);
            }
            case 'm':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint32_t n{};
                return read_number(n) && (result = n, true);
            }
            case 'M':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint64_t n{};
                if (!read_number(n))
                {
                    return false;
                }
                if (n > max_size)
                {
                    return fail("integer value overflow");
                }
                result = static_cast<std::size_t>(n);
                return true;
            }
            case '[':
            {
                if (!use_bjdata)
                {
                    break;
                }
                if (ndarray_pending)
                {
                    return fail("ndarray dimensional vector is not allowed");
                }
                json dims_json;
                if (!parse_array(dims_json))
                {
                    return false;
                }
                if (!dims_json.is_array())
                {
                    return fail("ndarray size must be an array");
                }
                const auto& dims = dims_json.get_ref<const json::array_t&>();
                std::vector<std::size_t> dim_sizes;
                dim_sizes.reserve(dims.size());
                std::uint64_t product = 1;
                for (const auto& d : dims)
                {
                    if (d.is_number_integer())
                    {
                        const auto v = d.get<json::number_integer_t>();
                        if (v < 0)
                        {
                            return fail("ndarray dimension must not be negative");
                        }
                        const auto u = static_cast<std::uint64_t>(v);
                        if (u > max_size)
                        {
                            return fail("ndarray dimension overflow");
                        }
                        dim_sizes.push_back(static_cast<std::size_t>(u));
                        if (u != 0 && product > max_size / u)
                        {
                            return fail("excessive ndarray size caused overflow");
                        }
                        product *= u;
                    }
                    else if (d.is_number_unsigned())
                    {
                        const auto u = d.get<json::number_unsigned_t>();
                        if (u > max_size)
                        {
                            return fail("ndarray dimension overflow");
                        }
                        dim_sizes.push_back(static_cast<std::size_t>(u));
                        if (u != 0 && product > max_size / u)
                        {
                            return fail("excessive ndarray size caused overflow");
                        }
                        product *= u;
                    }
                    else
                    {
                        return fail("ndarray dimension must be an integer");
                    }
                }
                // 1D row vectors are ordinary arrays in BJData, not ndarrays.
                if (dim_sizes.size() == 1 || (dim_sizes.size() == 2 && dim_sizes[0] == 1))
                {
                    result = dim_sizes.empty() ? 0 : dim_sizes.back();
                    return true;
                }
                for (const auto d : dim_sizes)
                {
                    if (d == 0)
                    {
                        result = 0;
                        return true;
                    }
                }
                if (product == static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                {
                    return fail("excessive ndarray size caused overflow");
                }
                ndarray_dims = std::move(dim_sizes);
                ndarray_pending = true;
                result = static_cast<std::size_t>(product);
                return true;
            }
            default:
                break;
        }
        char hex[5];
        std::snprintf(hex, sizeof hex, "0x%02X", prefix);
        err = std::string("expected length type specification after '#': ") + hex;
        return false;
    }

    // --- container size/type prefix (get_ubjson_size_type) ---
    // returns {size, type}; size == npos means no '#' count, type == 0 means
    // no '$' type. When neither prologue byte is present, the byte is
    // unget() and the caller parses it as the first element/key.
    bool read_size_type(std::size_t& size, std::uint8_t& type)
    {
        size = npos;
        type = 0;
        ndarray_pending = false;
        ndarray_dims.clear();
        ndarray_type = 0;
        std::uint8_t b{};
        if (!take_ignore_noop(b))
        {
            return false;
        }
        if (b == '$')
        {
            std::uint8_t t{};
            if (!take(t))
            {
                return false; // the type byte must not be a no-op
            }
            if (use_bjdata && bjdata_excluded(t))
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", t);
                err = std::string("marker ") + hex + " is not a permitted optimized array type";
                return false;
            }
            type = t;
            std::uint8_t hash{};
            if (!take_ignore_noop(hash) || hash != '#')
            {
                return fail("expected '#' after type information");
            }
            if (!read_size_value(size))
            {
                return false;
            }
            if (use_bjdata && ndarray_pending)
            {
                ndarray_type = type;
            }
            return true;
        }
        if (b == '#')
        {
            if (!read_size_value(size))
            {
                return false;
            }
            if (use_bjdata && ndarray_pending)
            {
                return fail("ndarray requires both type and size");
            }
            return true;
        }
        // neither '$' nor '#': the byte belongs to the first element
        unget();
        return true;
    }

    // excluded markers for the BJData '$' optimization (bjdx list)
    static bool bjdata_excluded(const std::uint8_t prefix) noexcept
    {
        switch (prefix)
        {
            case '[': case '{': case 'S': case 'H':
            case 'T': case 'F': case 'N': case 'Z':
                return true;
            default:
                return false;
        }
    }

    // --- 'S' string value (get_ubjson_string) ---
    // Note: unlike the size after '#', a no-op ('N') is NOT valid before a
    // string length prefix. The library's get_ubjson_string(get_char=true)
    // reads this byte directly; match that behavior.
    bool read_string_value(std::string& out)
    {
        std::uint8_t prefix{};
        if (!take(prefix))
        {
            return false;
        }
        switch (prefix)
        {
            case 'U':
            {
                std::uint8_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'i':
            {
                std::int8_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'I':
            {
                std::int16_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'l':
            {
                std::int32_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'L':
            {
                std::int64_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                if (static_cast<std::uint64_t>(len) > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                {
                    return fail("string length overflow");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'u':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint16_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'm':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint32_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'M':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint64_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                {
                    return fail("string length overflow");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            default:
                break;
        }
        char hex[5];
        std::snprintf(hex, sizeof hex, "0x%02X", prefix);
        err = std::string("expected length type specification for string: ") + hex;
        return false;
    }

    // --- high-precision number ('H'): length + decimal text, parsed like
    // the library's lexer (integer -> integer/unsigned, else float) ---
    bool read_high_precision(json& out)
    {
        std::size_t size = 0;
        if (!read_size_value(size))
        {
            return false;
        }
        std::string text;
        if (!take_bytes(size, text))
        {
            return false;
        }
        // Reuse the library lexer exactly like get_ubjson_high_precision_number:
        // it must accept/reject the same number grammar as from_ubjson.
        std::vector<char> number_vector(text.begin(), text.end());
        using ia_type = decltype(nlohmann::detail::input_adapter(number_vector));
        nlohmann::detail::lexer<nlohmann::json, ia_type> number_lexer(nlohmann::detail::input_adapter(number_vector), false);
        const auto result_number = number_lexer.scan();
        const auto result_remainder = number_lexer.scan();
        using token_type = typename nlohmann::detail::lexer_base<nlohmann::json>::token_type;
        if (result_remainder != token_type::end_of_input)
        {
            return fail("invalid high-precision number text");
        }
        switch (result_number)
        {
            case token_type::value_integer:
                out = number_lexer.get_number_integer();
                return true;
            case token_type::value_unsigned:
                out = number_lexer.get_number_unsigned();
                return true;
            case token_type::value_float:
            {
                const auto parsed_float = number_lexer.get_number_float();
                if (!std::isfinite(parsed_float))
                {
                    return fail("number overflow parsing high-precision number");
                }
                out = parsed_float;
                return true;
            }
            default:
                return fail("invalid high-precision number text");
        }
    }

    // --- 'C' char value: one ASCII byte -> single-char string ---
    bool read_char(json& out)
    {
        std::uint8_t c{};
        if (!take(c))
        {
            return false;
        }
        if (c > 127)
        {
            return fail("byte after 'C' must be in range 0x00..0x7F");
        }
        out = std::string(1, static_cast<char>(c));
        return true;
    }

    // --- '[' array (get_ubjson_array) ---
    // BJData ndarray: `[$<dtype># [dims...] <compact data>]` is decoded back
    // to the JData annotated object {_ArrayType_, _ArraySize_, _ArrayData_}.
    bool parse_ndarray(const std::size_t size, const std::uint8_t type, json& out)
    {
        const char* type_name = nullptr;
        switch (type)
        {
            case 'B': type_name = "byte";   break;
            case 'C': type_name = "char";   break;
            case 'D': type_name = "double"; break;
            case 'I': type_name = "int16";  break;
            case 'L': type_name = "int64";  break;
            case 'M': type_name = "uint64"; break;
            case 'U': type_name = "uint8";  break;
            case 'd': type_name = "single"; break;
            case 'i': type_name = "int8";   break;
            case 'l': type_name = "int32";  break;
            case 'm': type_name = "uint32"; break;
            case 'u': type_name = "uint16"; break;
            default: break;
        }
        if (type_name == nullptr)
        {
            ndarray_pending = false;
            ndarray_dims.clear();
            return fail("invalid ndarray element type");
        }

        // char/byte arrays carry their data as uint8 values.
        const std::uint8_t data_type = (type == 'C' || type == 'B') ? 'U' : type;

        json::object_t obj;
        obj["_ArrayType_"] = type_name;
        json::array_t sizes;
        for (const auto d : ndarray_dims)
        {
            sizes.push_back(json(static_cast<json::number_unsigned_t>(d)));
        }
        obj["_ArraySize_"] = std::move(sizes);

        json::array_t data;
        data.reserve(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            json el;
            if (!parse_typed(data_type, el))
            {
                ndarray_pending = false;
                ndarray_dims.clear();
                return false;
            }
            data.push_back(std::move(el));
        }
        obj["_ArrayData_"] = std::move(data);

        ndarray_pending = false;
        ndarray_dims.clear();
        out = std::move(obj);
        return true;
    }

    bool parse_array(json& out)
    {
        std::size_t size = 0;
        std::uint8_t type = 0;
        if (!read_size_type(size, type))
        {
            return false;
        }

        if (size != npos)
        {
            // BJData ndarray size vector: rebuild the JData annotated object
            if (use_bjdata && ndarray_pending)
            {
                return parse_ndarray(size, type, out);
            }

            // binary container in BJData: '$B#<len>' reads raw bytes
            if (use_bjdata && type == 'B')
            {
                std::string raw;
                if (!take_bytes(size, raw))
                {
                    return false;
                }
                out = json::binary_t(std::vector<std::uint8_t>(raw.begin(), raw.end()));
                return true;
            }

            json::array_t arr;
            arr.reserve(size);
            if (type != 0 && type != 'N')
            {
                for (std::size_t i = 0; i < size; ++i)
                {
                    json el;
                    if (!parse_typed(type, el))
                    {
                        return false;
                    }
                    arr.push_back(std::move(el));
                }
            }
            else
            {
                for (std::size_t i = 0; i < size; ++i)
                {
                    json el;
                    if (!parse_value(el))
                    {
                        return false;
                    }
                    arr.push_back(std::move(el));
                }
            }
            out = std::move(arr);
            return true;
        }

        // no count: elements run until ']'
        json::array_t arr;
        while (true)
        {
            std::uint8_t b{};
            if (!take_ignore_noop(b))
            {
                return false;
            }
            if (b == ']')
            {
                break;
            }
            json el;
            if (!parse_one(b, el))
            {
                return false;
            }
            arr.push_back(std::move(el));
        }
        out = std::move(arr);
        return true;
    }

    // decode a typed element (`type` is the marker from '$<type>'; the
    // element's own prefix is NOT consumed)
    bool parse_typed(const std::uint8_t type, json& out)
    {
        switch (type)
        {
            case 'T':
                out = true;
                return true;
            case 'F':
                out = false;
                return true;
            case 'Z':
                out = nullptr;
                return true;
            case 'U':
            {
                std::uint8_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'i':
            {
                std::int8_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'I':
            {
                std::int16_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'l':
            {
                std::int32_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'L':
            {
                std::int64_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'u':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint16_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'm':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint32_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'M':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint64_t v{};
                return read_number(v) && (out = v, true);
            }
            case 'd':
            {
                std::uint32_t bits{};
                if (!read_number(bits))
                {
                    return false;
                }
                float v{};
                std::memcpy(&v, &bits, sizeof(float));
                out = v;
                return true;
            }
            case 'D':
            {
                std::uint64_t bits{};
                if (!read_number(bits))
                {
                    return false;
                }
                double v{};
                std::memcpy(&v, &bits, sizeof(double));
                out = v;
                return true;
            }
            case 'h':
            {
                if (!use_bjdata)
                {
                    break;
                }
                double v{};
                return read_half(v) && (out = v, true);
            }
            case 'C':
            {
                return read_char(out);
            }
            case 'S':
            {
                std::string s;
                return read_string_value(s) && (out = std::move(s), true);
            }
            case 'H':
            {
                return read_high_precision(out);
            }
            case 'B':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint8_t v{};
                return read_number(v) && (out = v, true);
            }
            default:
                return fail("invalid optimized element type");
        }
        char hex[5];
        std::snprintf(hex, sizeof hex, "0x%02X", type);
        err = std::string("invalid optimized element type: ") + hex;
        return false;
    }

    // --- '{' object (get_ubjson_object) ---
    bool parse_object(json& out)
    {
        std::size_t size = 0;
        std::uint8_t type = 0;
        if (!read_size_type(size, type))
        {
            return false;
        }

        json::object_t obj;
        if (size != npos)
        {
            for (std::size_t i = 0; i < size; ++i)
            {
                std::string key;
                if (!read_string_value(key))
                {
                    return false;
                }
                json val;
                if (type != 0 && type != 'N')
                {
                    if (!parse_typed(type, val))
                    {
                        return false;
                    }
                }
                else if (!parse_value(val))
                {
                    return false;
                }
                obj[std::move(key)] = std::move(val);
            }
        }
        else
        {
            while (true)
            {
                std::uint8_t b{};
                if (!take_ignore_noop(b))
                {
                    return false;
                }
                if (b == '}')
                {
                    break;
                }
                // key length prefix
                std::string key;
                if (!read_string_from(b, key))
                {
                    return false;
                }
                json val;
                if (!parse_value(val))
                {
                    return false;
                }
                obj[std::move(key)] = std::move(val);
            }
        }
        out = std::move(obj);
        return true;
    }

    // object key: the length prefix byte is already consumed
    bool read_string_from(const std::uint8_t prefix, std::string& out)
    {
        switch (prefix)
        {
            case 'U':
            {
                std::uint8_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'i':
            {
                std::int8_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'I':
            {
                std::int16_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'l':
            {
                std::int32_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'L':
            {
                std::int64_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len < 0)
                {
                    return fail("string length must not be negative");
                }
                if (static_cast<std::uint64_t>(len) > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                {
                    return fail("string length overflow");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            case 'u':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint16_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'm':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint32_t len{};
                return read_number(len) && take_bytes(len, out);
            }
            case 'M':
            {
                if (!use_bjdata)
                {
                    break;
                }
                std::uint64_t len{};
                if (!read_number(len))
                {
                    return false;
                }
                if (len > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                {
                    return fail("string length overflow");
                }
                return take_bytes(static_cast<std::size_t>(len), out);
            }
            default:
                break;
        }
        char hex[5];
        std::snprintf(hex, sizeof hex, "0x%02X", prefix);
        err = std::string("expected length type specification for string: ") + hex;
        return false;
    }

    // --- main dispatch: `code` is the already-consumed marker byte ---
    bool parse_one(const std::uint8_t code, json& out)
    {
        const refl_detail::ubjson_load_entry entry = kUbjsonLoads[code];
        switch (static_cast<refl_detail::ubjson_payload_kind>(entry.kind))
        {
            case refl_detail::ubjson_payload_kind::boolean_true:
                out = true;
                return true;
            case refl_detail::ubjson_payload_kind::boolean_false:
                out = false;
                return true;
            case refl_detail::ubjson_payload_kind::null_fixed:
                out = nullptr;
                return true;
            case refl_detail::ubjson_payload_kind::noop:
                return parse_value(out); // skip and continue
            case refl_detail::ubjson_payload_kind::uint8:
            {
                std::uint8_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::int8:
            {
                std::int8_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::int16:
            {
                std::int16_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::int32:
            {
                std::int32_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::int64:
            {
                std::int64_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::uint16_bjd:
            {
                if (!use_bjdata)
                {
                    return fail("'u' is a BJData-only marker");
                }
                std::uint16_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::uint32_bjd:
            {
                if (!use_bjdata)
                {
                    return fail("'m' is a BJData-only marker");
                }
                std::uint32_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::uint64_bjd:
            {
                if (!use_bjdata)
                {
                    return fail("'M' is a BJData-only marker");
                }
                std::uint64_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::half_float:
            {
                if (!use_bjdata)
                {
                    return fail("'h' is a BJData-only marker");
                }
                double v{};
                return read_half(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::float32:
            {
                std::uint32_t bits{};
                if (!read_number(bits))
                {
                    return false;
                }
                float v{};
                std::memcpy(&v, &bits, sizeof(float));
                out = v;
                return true;
            }
            case refl_detail::ubjson_payload_kind::float64:
            {
                std::uint64_t bits{};
                if (!read_number(bits))
                {
                    return false;
                }
                double v{};
                std::memcpy(&v, &bits, sizeof(double));
                out = v;
                return true;
            }
            case refl_detail::ubjson_payload_kind::char_:
                return read_char(out);
            case refl_detail::ubjson_payload_kind::string_:
            {
                std::string s;
                return read_string_value(s) && (out = std::move(s), true);
            }
            case refl_detail::ubjson_payload_kind::high_precision:
                return read_high_precision(out);
            case refl_detail::ubjson_payload_kind::byte_bjd:
            {
                if (!use_bjdata)
                {
                    return fail("'B' is a BJData-only marker");
                }
                std::uint8_t v{};
                return read_number(v) && (out = v, true);
            }
            case refl_detail::ubjson_payload_kind::array_:
                return parse_array(out);
            case refl_detail::ubjson_payload_kind::object_:
                return parse_object(out);
            case refl_detail::ubjson_payload_kind::type_marker:
            case refl_detail::ubjson_payload_kind::count_marker:
                return fail("unexpected optimized-container marker at value position");
            default:
            {
                char hex[5];
                std::snprintf(hex, sizeof hex, "0x%02X", code);
                err = std::string("invalid UBJSON byte ") + hex;
                return false;
            }
        }
    }

    bool parse_value(json& out)
    {
        std::uint8_t b{};
        if (!take_ignore_noop(b))
        {
            return false;
        }
        return parse_one(b, out);
    }

    // BJData ndarray state: set while parsing `[$<type># [dims...] ...]`.
    // Consumed by parse_array(), which rebuilds the JData annotated object.
    bool ndarray_pending = false;
    std::vector<std::size_t> ndarray_dims;
    std::uint8_t ndarray_type = 0;
};


} // namespace rjson
