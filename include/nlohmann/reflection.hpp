//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++ (reflection extension)
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT
//
// Entry point for the refl2 static reflection serialization extension
// (feature/static-reflection branch). It is an ADD-ON header: it must be
// compiled together with the main <nlohmann/json.hpp> header, and requires a
// P2996 toolchain (g++-16 -std=c++26 -freflection).
//
// Usage:
//   #define JSON_USE_REFLECTION
//   #include <nlohmann/reflection.hpp>
//
// Without JSON_USE_REFLECTION the extension is inert and the library behaves
// byte-identically to the default build. This header must include json.hpp
// FIRST: reflection_to_json.hpp's JSON_HAS_CPP_26_REFLECTION macro is consumed
// by the main-library conversions headers it pulls in, so a standalone include
// of reflection_to_json.hpp before json.hpp trips an include-order error.

#pragma once

#include <nlohmann/json.hpp>
#include <nlohmann/reflection_to_json.hpp>
