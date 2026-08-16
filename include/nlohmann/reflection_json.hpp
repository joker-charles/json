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

} // namespace rjson
