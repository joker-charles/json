//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (reflection extension)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Entry point for the rjson reflection extension (feature/static-reflection
// branch): reflection-driven binary formats (MSGPACK / UBJSON / BSON / CBOR /
// BJData) and the tagged-union API surface (basic_json_reflection /
// reflection_iterator). It is an ADD-ON header, self-contained against the
// main library's internal basic_json::json_value layout, and requires a P2996
// toolchain (g++-16 -std=c++26 -freflection).
//
// Usage:
//   #include <nlohmann/reflection_binary.hpp>

#pragma once

#include <nlohmann/reflection_json.hpp>
