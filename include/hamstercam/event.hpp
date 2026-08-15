// SPDX-License-Identifier: MIT
#pragma once

#include "hamstercam/capture_device.hpp"
#include "hamstercam/error_reason.hpp"

namespace hamstercam {

// State transitions the state machine reports. Process-lifecycle
// events (startup, shutdown) are not among these -- they belong to main.cpp,
// which owns flag parsing and signal handling, not capture state.
enum class EventType {
    DeviceOpened,
    AwaitingProducer,
    StreamingStarted,
    DeviceLost,
    Reconnecting,
    StallDetected,
    StallCleared,
    FormatChanged,
};

struct Event {
    EventType type;
    StreamFormat format{};           // StreamingStarted, FormatChanged (new)
    StreamFormat previous_format{};  // FormatChanged (old)
    ErrorReason reason = ErrorReason::Count;  // DeviceLost
};

// Carries state changes only, never per-frame events: at 15 fps a log line
// per frame is unreadable and fills an SD card. The real sink formats these
// as JSON lines; tests use one that just records them.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(const Event& event) = 0;
};

}  // namespace hamstercam
