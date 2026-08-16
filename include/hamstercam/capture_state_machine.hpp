// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>

#include "hamstercam/backoff.hpp"
#include "hamstercam/capture_device.hpp"
#include "hamstercam/capture_state.hpp"
#include "hamstercam/clock.hpp"
#include "hamstercam/daemon_config.hpp"
#include "hamstercam/error_reason.hpp"
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

    // The post-release poll: entered only via begin_release_cycle(), reported
    // as awaiting_producer (not a distinct state -- see begin_release_cycle),
    // but mechanically different from step_awaiting_producer() because the fd
    // must never be held between attempts here.
    StepOutcome step_recovering();

    // Shared by step_awaiting_producer() (initial configure) and
    // step_recovering() (post-release reconfigure).
    StepOutcome enter_configuring();

    void handle_frame(const FrameDescriptor& frame);

    // Tracks buffer arrival rate over a rolling window while streaming, and
    // flags a sustained excursion well above expected_fps -- the signature of
    // a producer being truncated into the wrong buffer size, which floods the
    // device with fragments rather than causing frames to stop. Detection
    // only: never releases the device itself.
    void check_arrival_rate(std::chrono::steady_clock::time_point now);

    // Releases the device (stop streaming, unmap, close) and moves to
    // awaiting_producer to wait out a disruption -- either the stall
    // threshold tripping or a payload size that no longer matches the
    // negotiated format. Both are the daemon's own attachment pinning a
    // format nothing is writing correctly anymore, so both recover the same
    // way: let go and wait for a producer to (re)establish one.
    void begin_release_cycle(ErrorReason reason);

    void lose_device();
    void reset_backoffs();

    ICaptureDevice& device_;
    Clock& clock_;
    EventSink& events_;
    CaptureStats& stats_;
    DaemonConfig config_;

    CaptureState state_ = CaptureState::DeviceAbsent;

    // Separate instances, not one backoff with a state-dependent ceiling:
    // device_absent and awaiting_producer have different urgency and must be
    // able to grow independently of each other.
    Backoff device_absent_backoff_;
    Backoff awaiting_producer_backoff_;

    std::chrono::steady_clock::time_point next_attempt_at_{};
    std::chrono::steady_clock::time_point last_frame_at_{};
    bool have_last_frame_ = false;

    // True from begin_release_cycle() until a producer is reacquired or the
    // device is lost outright. Distinguishes the post-release poll (transient
    // open/query/close every attempt) from the ordinary pre-stream wait
    // (fd opened once, held for the duration) even though both report the
    // same awaiting_producer state value.
    bool recovering_ = false;

    StreamFormat current_format_{};
    bool has_streamed_before_ = false;

    std::chrono::milliseconds stall_threshold_{};

    std::chrono::steady_clock::time_point rate_window_start_{};
    std::uint32_t rate_window_frame_count_ = 0;
    bool rate_anomaly_active_ = false;
};

}  // namespace hamstercam
