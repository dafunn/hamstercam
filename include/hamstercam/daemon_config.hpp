// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>

namespace hamstercam {

// Only the settings the capture state machine itself needs. Device path,
// metrics-addr and log-level belong to main.cpp's flag parsing, not here --
// the state machine operates on an already-constructed ICaptureDevice.
//
// Note there is deliberately no width, height, or capture-format setting:
// those are observed from whatever is writing to the device, never requested.
struct DaemonConfig {
    std::uint32_t expected_fps = 15;
    std::uint32_t stall_multiple = 10;
    std::uint32_t buffer_count = 4;
    std::chrono::milliseconds reconnect_initial_backoff{200};
    std::chrono::milliseconds reconnect_max_backoff{30000};
};

}  // namespace hamstercam
