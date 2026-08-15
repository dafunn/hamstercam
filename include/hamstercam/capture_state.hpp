// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace hamstercam {

// These values are exported directly as the hamstercam_capture_state gauge,
// so they are a public interface: dashboards and alerts key off the numbers.
// Renumbering silently breaks every consumer.
enum class CaptureState {
    DeviceAbsent = 0,
    AwaitingProducer = 1,
    Configuring = 2,
    Streaming = 3,
    Stalled = 4,
};

constexpr std::string_view to_string(CaptureState state) {
    switch (state) {
        case CaptureState::DeviceAbsent:
            return "device_absent";
        case CaptureState::AwaitingProducer:
            return "awaiting_producer";
        case CaptureState::Configuring:
            return "configuring";
        case CaptureState::Streaming:
            return "streaming";
        case CaptureState::Stalled:
            return "stalled";
    }
    return "unknown";
}

}  // namespace hamstercam
