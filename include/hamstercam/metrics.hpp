// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

#include "hamstercam/stats.hpp"

namespace hamstercam {

struct BuildInfo {
    std::string version;
    std::string git_sha;
};

// Prometheus text exposition, format version 0.0.4.
std::string render_metrics(const CaptureStats& stats, const BuildInfo& build, double uptime_seconds,
                            std::uint64_t resident_bytes);

}  // namespace hamstercam
