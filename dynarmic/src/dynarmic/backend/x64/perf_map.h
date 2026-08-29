/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <string_view>

#include <mcl/bit_cast.hpp>

namespace Dynarmic::Backend::X64 {

namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail

/* Whether a perf map is actually being written.  Callers that have to build
 * the name (fmt::format plus a std::string, once per compiled block) ask first
 * — without a perf map the name is formatted and thrown away, and at 600k
 * blocks per startup that is real time. */
bool PerfMapEnabled();

template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) {
    detail::PerfMapRegister(mcl::bit_cast<const void*>(start), end, friendly_name);
}

void PerfMapClear();

}  // namespace Dynarmic::Backend::X64
