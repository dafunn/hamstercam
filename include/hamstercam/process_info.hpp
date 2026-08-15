// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace hamstercam {

// Reads /proc/self/status; returns 0 if unavailable.
std::uint64_t resident_set_bytes();

}  // namespace hamstercam
