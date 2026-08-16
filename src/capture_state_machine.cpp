// SPDX-License-Identifier: MIT
#include "hamstercam/capture_state_machine.hpp"

#include <algorithm>

namespace hamstercam {

namespace {
// A sustained excursion above this multiple of expected_fps, held for at
// least this long, flags a truncation storm (a producer's frames being
// silently cut down to a mismatched buffer size, which makes it retry and
// write far more often, not less) rather than a normal frame. Constants, not
// configuration: this only needs to distinguish "wildly off" from "normal,"
// not track a real rate.
constexpr double kRateAnomalyFactor = 5.0;
constexpr std::chrono::seconds kRateAnomalyWindow{2};
}  // namespace

CaptureStateMachine::CaptureStateMachine(ICaptureDevice& device, Clock& clock, EventSink& events,
                                          CaptureStats& stats, DaemonConfig config)
    : device_(device),
      clock_(clock),
      events_(events),
      stats_(stats),
      config_(config),
      device_absent_backoff_(config.reconnect_initial_backoff, config.reconnect_max_backoff),
      awaiting_producer_backoff_(config.reconnect_initial_backoff, config.awaiting_producer_max_backoff) {
    next_attempt_at_ = clock_.steady_now();

    const double expected_interval_seconds =
        config_.expected_fps > 0 ? 1.0 / static_cast<double>(config_.expected_fps) : 1.0;
    stall_threshold_ = std::chrono::milliseconds(static_cast<std::int64_t>(
        expected_interval_seconds * static_cast<double>(config_.stall_multiple) * 1000.0));

    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step(std::chrono::milliseconds poll_timeout) {
    switch (state_) {
        case CaptureState::DeviceAbsent:
            return step_device_absent();
        case CaptureState::AwaitingProducer:
            return recovering_ ? step_recovering() : step_awaiting_producer();
        case CaptureState::Configuring:
        case CaptureState::Stalled:
            // Both transient: enter_configuring() and begin_release_cycle()
            // always leave these states before returning, so step() should
            // never observe either one.
            return StepOutcome{};
        case CaptureState::Streaming:
            return step_streaming(poll_timeout);
    }
    return StepOutcome{};
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step_device_absent() {
    const auto now = clock_.steady_now();
    if (now < next_attempt_at_) {
        return StepOutcome{std::chrono::duration_cast<std::chrono::milliseconds>(next_attempt_at_ - now)};
    }

    if (device_.open() == OpenResult::Opened) {
        reset_backoffs();
        state_ = CaptureState::AwaitingProducer;
        stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
        events_.emit(Event{EventType::DeviceOpened});
        events_.emit(Event{EventType::AwaitingProducer});
        return StepOutcome{};
    }

    stats_.record_error(ErrorReason::Open);
    const auto delay = device_absent_backoff_.next();
    next_attempt_at_ = now + delay;
    return StepOutcome{delay};
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step_awaiting_producer() {
    const auto now = clock_.steady_now();
    if (now < next_attempt_at_) {
        return StepOutcome{std::chrono::duration_cast<std::chrono::milliseconds>(next_attempt_at_ - now)};
    }

    StreamFormat format{};
    const auto result = device_.query_format(format);

    if (result == FormatResult::NoProducer) {
        const auto delay = awaiting_producer_backoff_.next();
        next_attempt_at_ = now + delay;
        return StepOutcome{delay};
    }

    if (result == FormatResult::Error) {
        stats_.record_error(ErrorReason::Format);
        const auto delay = awaiting_producer_backoff_.next();
        next_attempt_at_ = now + delay;
        return StepOutcome{delay};
    }

    current_format_ = format;
    const auto outcome = enter_configuring();

    if (state_ == CaptureState::Streaming) {
        if (has_streamed_before_) {
            stats_.reconnects.fetch_add(1, std::memory_order_relaxed);
            events_.emit(Event{EventType::Reconnecting});
        } else {
            has_streamed_before_ = true;
            events_.emit(Event{EventType::StreamingStarted, current_format_});
        }
    }

    return outcome;
}

CaptureStateMachine::StepOutcome CaptureStateMachine::enter_configuring() {
    state_ = CaptureState::Configuring;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);

    if (device_.start_streaming(config_.buffer_count) == StreamStartResult::BufferError) {
        stats_.record_error(ErrorReason::Buffer);
        state_ = CaptureState::AwaitingProducer;
        stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
        const auto delay = awaiting_producer_backoff_.next();
        next_attempt_at_ = clock_.steady_now() + delay;
        return StepOutcome{delay};
    }

    reset_backoffs();
    stats_.set_stream_info(current_format_);
    have_last_frame_ = false;
    rate_window_start_ = clock_.steady_now();
    rate_window_frame_count_ = 0;
    rate_anomaly_active_ = false;

    state_ = CaptureState::Streaming;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    return StepOutcome{};
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step_streaming(std::chrono::milliseconds poll_timeout) {
    FrameDescriptor frame{};
    const auto result = device_.dequeue_frame(poll_timeout, frame);

    switch (result) {
        case DequeueResult::Frame:
            handle_frame(frame);
            check_arrival_rate(frame.captured_at);
            // sizeimage is exact for an uncompressed format and only an
            // upper bound for a compressed one, so a mismatch is only
            // meaningful when compressed is false.
            if (!current_format_.compressed && frame.bytes_used != current_format_.size_image) {
                begin_release_cycle(ErrorReason::Geometry);
            }
            return StepOutcome{};

        case DequeueResult::Timeout: {
            const auto now = clock_.steady_now();
            if (have_last_frame_ && now - last_frame_at_ >= stall_threshold_) {
                begin_release_cycle(ErrorReason::Timeout);
            }
            return StepOutcome{};
        }

        case DequeueResult::Error:
            stats_.record_error(ErrorReason::Dequeue);
            return StepOutcome{};

        case DequeueResult::DeviceLost:
            lose_device();
            return StepOutcome{};
    }
    return StepOutcome{};
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step_recovering() {
    const auto now = clock_.steady_now();
    if (now < next_attempt_at_) {
        return StepOutcome{std::chrono::duration_cast<std::chrono::milliseconds>(next_attempt_at_ - now)};
    }

    // Transient probe: open, query, close -- on every attempt, including a
    // failed one. A fd held open here would itself freeze whatever format it
    // just observed, which is the exact defect this replaces.
    if (device_.open() != OpenResult::Opened) {
        lose_device();
        return StepOutcome{};
    }

    StreamFormat format{};
    const auto result = device_.query_format(format);

    if (result != FormatResult::Known) {
        device_.close();
        if (result == FormatResult::Error) stats_.record_error(ErrorReason::Format);
        const auto delay = awaiting_producer_backoff_.next();
        next_attempt_at_ = now + delay;
        return StepOutcome{delay};
    }

    // A format was observed: this fd becomes the real one rather than being
    // closed and reopened a third time.
    const StreamFormat previous = current_format_;
    current_format_ = format;
    const auto outcome = enter_configuring();

    if (state_ == CaptureState::Streaming) {
        recovering_ = false;
        // Ordering matters here: stall_cleared always first, format_changed
        // only if the driver's answer actually differs (a reacquire is not
        // by itself evidence of a change -- every producer restart runs this
        // cycle, most come back identical), then streaming_started.
        events_.emit(Event{EventType::StallCleared});
        if (!(format == previous)) {
            events_.emit(Event{EventType::FormatChanged, format, previous});
        }
        // Never a reconnection: the device was released deliberately, not
        // lost. has_streamed_before_ is already true by the time this path
        // can run, so the ordinary awaiting_producer branch would otherwise
        // misreport this as one.
        events_.emit(Event{EventType::StreamingStarted, current_format_});
    } else {
        // enter_configuring() failed (buffer error) and already queued a
        // backoff back in awaiting_producer. Close here too: this path must
        // never leave a fd open between polls, same as an EINVAL probe above.
        device_.close();
    }

    return outcome;
}

void CaptureStateMachine::handle_frame(const FrameDescriptor& frame) {
    device_.requeue_frame(frame.buffer_index);

    stats_.frames_received.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_received.fetch_add(frame.bytes_used, std::memory_order_relaxed);
    stats_.last_frame_unixtime.store(
        std::chrono::duration_cast<std::chrono::seconds>(clock_.wall_now().time_since_epoch()).count(),
        std::memory_order_relaxed);

    if (have_last_frame_) {
        const std::chrono::duration<double> interval = frame.captured_at - last_frame_at_;
        stats_.frame_interval_seconds.observe(interval.count());
    }
    last_frame_at_ = frame.captured_at;
    have_last_frame_ = true;
}

void CaptureStateMachine::check_arrival_rate(std::chrono::steady_clock::time_point now) {
    ++rate_window_frame_count_;
    const auto elapsed = now - rate_window_start_;
    if (elapsed < kRateAnomalyWindow) return;

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double rate = static_cast<double>(rate_window_frame_count_) / seconds;
    const double threshold = static_cast<double>(config_.expected_fps) * kRateAnomalyFactor;

    if (config_.expected_fps > 0 && rate > threshold) {
        // Detection only -- do not release the device here. If another
        // opener is pinning the format, releasing changes nothing; if it's
        // this daemon's own doing, the payload-size check already handles
        // it. This is the detector for the case that isn't either of those.
        if (!rate_anomaly_active_) {
            stats_.record_error(ErrorReason::Geometry);
            rate_anomaly_active_ = true;
        }
    } else {
        rate_anomaly_active_ = false;
    }

    rate_window_start_ = now;
    rate_window_frame_count_ = 0;
}

void CaptureStateMachine::begin_release_cycle(ErrorReason reason) {
    stats_.stalls.fetch_add(1, std::memory_order_relaxed);
    stats_.record_error(reason);

    // Momentary: entered and left within this call, never a resting value.
    // The durable signal for "this happened" is stalls_total, not a gauge a
    // scrape will essentially never sample here.
    state_ = CaptureState::Stalled;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    events_.emit(Event{EventType::StallDetected});

    // The daemon's own attachment is what pins the wrong (or now-silent)
    // format onto the device; releasing it completely is the only way a
    // producer's negotiation becomes visible again.
    device_.stop_streaming();
    device_.close();

    state_ = CaptureState::AwaitingProducer;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    recovering_ = true;
    awaiting_producer_backoff_.reset();
    next_attempt_at_ = clock_.steady_now();
    have_last_frame_ = false;
}

void CaptureStateMachine::lose_device() {
    stats_.record_error(ErrorReason::DeviceLost);
    device_.stop_streaming();
    device_.close();
    events_.emit(Event{EventType::DeviceLost});

    state_ = CaptureState::DeviceAbsent;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    recovering_ = false;
    reset_backoffs();
    next_attempt_at_ = clock_.steady_now();
    have_last_frame_ = false;
}

void CaptureStateMachine::reset_backoffs() {
    device_absent_backoff_.reset();
    awaiting_producer_backoff_.reset();
}

}  // namespace hamstercam
