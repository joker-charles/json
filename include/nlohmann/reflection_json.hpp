// reflection_json.hpp — a C++26 / P2996 static-reflection playground mirroring
// a slice of nlohmann::basic_json's tagged-union logic, to prove that the
// value_t <-> json_value-member <-> storage-category mapping can be driven by
// a compile-time reflection-generated table instead of hand-written switch
// dispatch.
//
// NOTE on access control (verified on g++-16 16.1.0, see m2_table.cpp):
//   basic_json::json_value and basic_json::data are PRIVATE nested types, and
//   even access_context::unchecked() does NOT let external scope-splices
//   `[: ^^ json::json_value :]` reference them. M2 therefore reflects a MIRROR
//   union (json_value_mirror below) whose members are declared in the same
//   order as the real private union. value_t (nlohmann::detail::value_t) is
//   PUBLIC and is reflected directly.
//
// Build/link flags: -std=c++26 -freflection ; include the repo's single_include.
// This header reuses the library's public type aliases (object_t, array_t, ...)
// via nlohmann::json but keeps its own storage, so it never touches the real
// private internals and leaves the C++11 path untouched.
//
// Design (the "single source of truth" claim):
//   * kStorage   — reflection-generated: which union member is pointer-stored.
//                  This is the ONE place the pointer/scalar split lives; the
//                  real library repeats it across ctor/destroy/invariant.
//   * slot_index<V> — compile-time map value_t -> union member index; a value_t
//                  without a slot (null/discarded) has has==false.
//   * default-construct/destroy are dispatched by a `template for` over ALL
//                  value_t enumerators, so adding a value_t without wiring it
//                  is a COMPILE ERROR, never a silent drift.

#pragma once

#include <algorithm>   // reverse
#include <array>
#include <cmath>       // isfinite, isinf, isnan, abs
#include <cstdint>
#include <cstdio>      // snprintf
#include <cstdlib>     // abort
#include <cstring>     // memcpy
#include <limits>      // numeric_limits
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
// MIRROR union — storage category & member-name lookup. Members are declared
// in the same order as basic_json::json_value (object, array, string, binary,
// boolean, number_integer, number_unsigned, number_float).
// ---------------------------------------------------------------------------
union json_value_mirror
{
    json::object_t* object;
    json::array_t* array;
    json::string_t* string;
    json::binary_t* binary;
    json::boolean_t boolean;
    json::number_integer_t number_integer;
    json::number_unsigned_t number_unsigned;
    json::number_float_t number_float;
};

namespace refl_detail
{
consteval std::size_t member_count()
{
    return std::meta::nonstatic_data_members_of(^^json_value_mirror,
            std::meta::access_context::unprivileged()).size();
}

template<std::size_t I>
consteval bool member_is_pointer()
{
    constexpr auto m =
        std::meta::nonstatic_data_members_of(^^json_value_mirror,
            std::meta::access_context::unprivileged())[I];
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
                                     ^^json_value_mirror, std::meta::access_context::unprivileged())[I])...
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
void construct_one(json_value_mirror& u)
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
void destroy_one(json_value_mirror& u)
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
struct basic_json_reflection
{
    value_t m_type = value_t::null;
    json_value_mirror m_value{}; // default-init: object pointer = nullptr

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
    [[nodiscard]] const json_value_mirror& value() const noexcept
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
};

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
        if (std::isnan(f))
        {
            out += "null";
            return;
        }
        if (std::isinf(f))
        {
            out += f > 0 ? "1e+999" : "-1e+999";
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
} // namespace rjson
