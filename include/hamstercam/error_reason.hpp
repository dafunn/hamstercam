// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

namespace hamstercam {

// Fixed set backing hamstercam_capture_errors_total{reason}. Deliberately not
// derived from errno or any other open-ended source: an unbounded label set
// multiplies into a new time series per distinct value and will eventually
// overwhelm the metrics store.
enum class ErrorReason : std::size_t {
    Open = 0,
    Format,
    Buffer,
    Timeout,
    Dequeue,
    DeviceLost,
    Count,
};

constexpr std::string_view to_string(ErrorReason reason) {
    switch (reason) {
        case ErrorReason::Open:
            return "open";
        case ErrorReason::Format:
            return "format";
        case ErrorReason::Buffer:
            return "buffer";
        case ErrorReason::Timeout:
            return "timeout";
        case ErrorReason::Dequeue:
            return "dequeue";
        case ErrorReason::DeviceLost:
            return "device_lost";
        case ErrorReason::Count:
            break;
    }
    return "unknown";
}

constexpr std::size_t kErrorReasonCount = static_cast<std::size_t>(ErrorReason::Count);

}  // namespace hamstercam
