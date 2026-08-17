// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

namespace hamstercam {

// Backing store for the exporter's metrics: written by the progress-socket
// thread, read by the metrics HTTP thread.
//
// The counters are stored from ffmpeg's own cumulative values, never
// accumulated here, so a producer restart shows up as the decrease it is.
struct ProducerStats {
    std::atomic<std::uint64_t> frames_total{0};
    std::atomic<double> fps{0.0};
    std::atomic<std::uint64_t> dropped_frames_total{0};
    std::atomic<std::uint64_t> duplicated_frames_total{0};
    std::atomic<double> speed_ratio{0.0};
    std::atomic<bool> connected{false};
    std::atomic<std::int64_t> last_update_unixtime{0};
};

}  // namespace hamstercam
