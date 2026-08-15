// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>

#include "hamstercam/backoff.hpp"
#include "hamstercam/capture_device.hpp"
#include "hamstercam/capture_state.hpp"
#include "hamstercam/clock.hpp"
#include "hamstercam/daemon_config.hpp"
#include "hamstercam/event.hpp"
#include "hamstercam/stats.hpp"

namespace hamstercam {

// Drives one ICaptureDevice through the capture lifecycle. Holds no threads
// and does no sleeping itself: step() returns immediately except where
// dequeue_frame() blocks internally on poll(), and hands the caller a
// recommended wait instead of sleeping on its own. That keeps every timing
// rule -- backoff, stall thresholds, format re-checks -- testable against a
// fake clock rather than requiring tests to run in real time.
class CaptureStateMachine {
public:
    CaptureStateMachine(ICaptureDevice& device, Clock& clock, EventSink& events, CaptureStats& stats,
                         DaemonConfig config);

    struct StepOutcome {
        // How long the caller should wait before calling step() again.
        // Zero means call again immediately -- either progress was made, or
        // the device's own poll() already provided the wait. Non-zero means
        // the state machine is backing off; the caller must sleep
        // interruptibly, or a long backoff will delay shutdown past the
        // couple of seconds systemd is willing to wait after SIGTERM.
        std::chrono::milliseconds recommended_wait{0};
    };

    StepOutcome step(std::chrono::milliseconds poll_timeout);

    CaptureState state() const { return state_; }

private:
    StepOutcome step_device_absent();
    StepOutcome step_awaiting_producer();
    StepOutcome step_streaming(std::chrono::milliseconds poll_timeout);
    StepOutcome step_stalled(std::chrono::milliseconds poll_timeout);

    // Shared by step_awaiting_producer() (initial configure) and
    // step_streaming() (post format-change reconfigure).
    StepOutcome enter_configuring();

    void handle_frame(const FrameDescriptor& frame);
    StepOutcome maybe_check_format();
    void enter_stalled();
    void lose_device();

    ICaptureDevice& device_;
    Clock& clock_;
    EventSink& events_;
    CaptureStats& stats_;
    DaemonConfig config_;

    CaptureState state_ = CaptureState::DeviceAbsent;
    Backoff backoff_;

    std::chrono::steady_clock::time_point next_attempt_at_{};
    std::chrono::steady_clock::time_point last_frame_at_{};
    bool have_last_frame_ = false;
    std::chrono::steady_clock::time_point last_format_check_at_{};

    StreamFormat current_format_{};
    bool has_streamed_before_ = false;

    std::chrono::milliseconds stall_threshold_{};
};

}  // namespace hamstercam
