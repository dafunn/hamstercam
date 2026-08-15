// SPDX-License-Identifier: MIT
#include "hamstercam/capture_state_machine.hpp"

#include <algorithm>

namespace hamstercam {

namespace {
// Cadence for the periodic VIDIOC_G_FMT re-check while streaming. A writer
// can restart with different settings at any moment, and nothing notifies us
// when it does -- the format simply changes underneath. Cheap ioctl, but
// checked on a timer rather than per frame to avoid a syscall at frame rate.
constexpr std::chrono::seconds kFormatCheckInterval{1};
}  // namespace

CaptureStateMachine::CaptureStateMachine(ICaptureDevice& device, Clock& clock, EventSink& events,
                                          CaptureStats& stats, DaemonConfig config)
    : device_(device),
      clock_(clock),
      events_(events),
      stats_(stats),
      config_(config),
      backoff_(config.reconnect_initial_backoff, config.reconnect_max_backoff) {
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
            return step_awaiting_producer();
        case CaptureState::Configuring:
            // Transient: enter_configuring() always leaves this state before
            // returning, so step() should never observe it.
            return StepOutcome{};
        case CaptureState::Streaming:
            return step_streaming(poll_timeout);
        case CaptureState::Stalled:
            return step_stalled(poll_timeout);
    }
    return StepOutcome{};
}

CaptureStateMachine::StepOutcome CaptureStateMachine::step_device_absent() {
    const auto now = clock_.steady_now();
    if (now < next_attempt_at_) {
        return StepOutcome{std::chrono::duration_cast<std::chrono::milliseconds>(next_attempt_at_ - now)};
    }

    if (device_.open() == OpenResult::Opened) {
        backoff_.reset();
        state_ = CaptureState::AwaitingProducer;
        stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
        events_.emit(Event{EventType::DeviceOpened});
        events_.emit(Event{EventType::AwaitingProducer});
        return StepOutcome{};
    }

    stats_.record_error(ErrorReason::Open);
    const auto delay = backoff_.next();
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
        const auto delay = backoff_.next();
        next_attempt_at_ = now + delay;
        return StepOutcome{delay};
    }

    if (result == FormatResult::Error) {
        stats_.record_error(ErrorReason::Format);
        const auto delay = backoff_.next();
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
        const auto delay = backoff_.next();
        next_attempt_at_ = clock_.steady_now() + delay;
        return StepOutcome{delay};
    }

    backoff_.reset();
    stats_.set_stream_info(current_format_);
    have_last_frame_ = false;
    last_format_check_at_ = clock_.steady_now();

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
            return maybe_check_format();

        case DequeueResult::Timeout: {
            const auto now = clock_.steady_now();
            if (have_last_frame_ && now - last_frame_at_ >= stall_threshold_) {
                enter_stalled();
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

CaptureStateMachine::StepOutcome CaptureStateMachine::step_stalled(std::chrono::milliseconds poll_timeout) {
    FrameDescriptor frame{};
    const auto result = device_.dequeue_frame(poll_timeout, frame);

    switch (result) {
        case DequeueResult::Frame:
            handle_frame(frame);
            state_ = CaptureState::Streaming;
            stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
            events_.emit(Event{EventType::StallCleared});
            return StepOutcome{};

        case DequeueResult::Timeout:
            return StepOutcome{};

        case DequeueResult::Error:
            stats_.record_error(ErrorReason::Dequeue);
            return StepOutcome{};

        case DequeueResult::DeviceLost:
            lose_device();
            return StepOutcome{};
    }
    return StepOutcome{};
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

CaptureStateMachine::StepOutcome CaptureStateMachine::maybe_check_format() {
    const auto now = clock_.steady_now();
    if (now - last_format_check_at_ < kFormatCheckInterval) return StepOutcome{};
    last_format_check_at_ = now;

    StreamFormat format{};
    if (device_.query_format(format) != FormatResult::Known) return StepOutcome{};
    if (format == current_format_) return StepOutcome{};

    // Reconfiguring here never touches has_streamed_before_/reconnects_total:
    // the device never left, so this is not a reconnection.
    const StreamFormat previous = current_format_;
    device_.stop_streaming();
    current_format_ = format;
    events_.emit(Event{EventType::FormatChanged, format, previous});
    return enter_configuring();
}

void CaptureStateMachine::enter_stalled() {
    state_ = CaptureState::Stalled;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    stats_.stalls.fetch_add(1, std::memory_order_relaxed);
    stats_.record_error(ErrorReason::Timeout);
    events_.emit(Event{EventType::StallDetected});
}

void CaptureStateMachine::lose_device() {
    stats_.record_error(ErrorReason::DeviceLost);
    device_.stop_streaming();
    device_.close();
    events_.emit(Event{EventType::DeviceLost});

    state_ = CaptureState::DeviceAbsent;
    stats_.capture_state.store(static_cast<int>(state_), std::memory_order_relaxed);
    backoff_.reset();
    next_attempt_at_ = clock_.steady_now();
    have_last_frame_ = false;
}

}  // namespace hamstercam
