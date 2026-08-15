// SPDX-License-Identifier: MIT
#include "hamstercam/capture_state_machine.hpp"

#include <gtest/gtest.h>

#include "fake_capture_device.hpp"
#include "fake_clock.hpp"
#include "recording_event_sink.hpp"

using namespace std::chrono_literals;
using namespace hamstercam;
using hamstercam::testing::FakeCaptureDevice;
using hamstercam::testing::FakeClock;
using hamstercam::testing::RecordingEventSink;

namespace {

DaemonConfig test_config() {
    DaemonConfig config;
    config.expected_fps = 10;         // 100 ms nominal interval
    config.stall_multiple = 3;        // stall at 300 ms of silence
    config.buffer_count = 4;
    config.reconnect_initial_backoff = 50ms;
    config.reconnect_max_backoff = 400ms;
    return config;
}

StreamFormat test_format(std::uint32_t width = 1280) {
    StreamFormat format;
    format.width = width;
    format.height = 720;
    format.pixel_format_fourcc = 0x47504A4D;  // 'MJPG'
    return format;
}

class CaptureStateMachineTest : public ::testing::Test {
protected:
    FakeClock clock;
    FakeCaptureDevice device{clock};
    RecordingEventSink events;
    CaptureStats stats;
    CaptureStateMachine machine{device, clock, events, stats, test_config()};

    // Drives the machine from DeviceAbsent to Streaming: open succeeds,
    // format is known immediately, buffers start cleanly.
    void reach_streaming(const StreamFormat& format = test_format()) {
        device.set_open_result(OpenResult::Opened);
        device.set_format_result(FormatResult::Known);
        device.set_format(format);

        auto outcome = machine.step(10ms);  // DeviceAbsent -> AwaitingProducer
        clock.advance(outcome.recommended_wait);
        outcome = machine.step(10ms);  // AwaitingProducer -> Streaming
        clock.advance(outcome.recommended_wait);

        ASSERT_EQ(machine.state(), CaptureState::Streaming);
    }

    std::size_t count_events(EventType type) const {
        std::size_t n = 0;
        for (const auto& e : events.events) {
            if (e.type == type) ++n;
        }
        return n;
    }
};

}  // namespace

// --- device absent is not an error, retried with bounded backoff ---

TEST_F(CaptureStateMachineTest, DeviceAbsentRetriesWithGrowingBackoff) {
    EXPECT_EQ(machine.state(), CaptureState::DeviceAbsent);

    auto outcome = machine.step(10ms);
    EXPECT_EQ(machine.state(), CaptureState::DeviceAbsent);
    EXPECT_EQ(outcome.recommended_wait, 50ms);
    EXPECT_EQ(stats.error_count(ErrorReason::Open), 1u);
    EXPECT_EQ(device.open_call_count(), 1);

    // Calling again before the backoff elapses must not retry, or waiting for
    // an absent device becomes a busy-spin.
    auto premature = machine.step(10ms);
    EXPECT_EQ(device.open_call_count(), 1);
    EXPECT_GT(premature.recommended_wait, 0ms);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    EXPECT_EQ(outcome.recommended_wait, 100ms);
    EXPECT_EQ(stats.error_count(ErrorReason::Open), 2u);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    EXPECT_EQ(outcome.recommended_wait, 200ms);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    EXPECT_EQ(outcome.recommended_wait, 400ms);  // capped at reconnect_max_backoff
}

// --- present but no producer is not an error; logs exactly once ---

TEST_F(CaptureStateMachineTest, AwaitingProducerLogsOnlyOnceWhileWaiting) {
    device.set_open_result(OpenResult::Opened);
    device.set_format_result(FormatResult::NoProducer);

    auto outcome = machine.step(10ms);
    ASSERT_EQ(machine.state(), CaptureState::AwaitingProducer);
    EXPECT_EQ(count_events(EventType::AwaitingProducer), 1u);

    for (int i = 0; i < 5; ++i) {
        clock.advance(outcome.recommended_wait);
        outcome = machine.step(10ms);
        EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
    }

    EXPECT_EQ(count_events(EventType::AwaitingProducer), 1u);
    EXPECT_EQ(stats.error_count(ErrorReason::Open), 0u);
}

// --- happy path: reaching streaming, frames counted, buffers requeued ---

TEST_F(CaptureStateMachineTest, ReachingStreamingEmitsStreamingStartedAndPublishesFormat) {
    const auto format = test_format();
    reach_streaming(format);

    ASSERT_EQ(count_events(EventType::StreamingStarted), 1u);
    EXPECT_TRUE(events.events.back().format == format);
    EXPECT_EQ(device.last_buffer_count_requested(), 4u);

    StreamFormat published{};
    ASSERT_TRUE(stats.stream_info(published));
    EXPECT_TRUE(published == format);
    EXPECT_EQ(stats.capture_state.load(), static_cast<int>(CaptureState::Streaming));
}

TEST_F(CaptureStateMachineTest, FramesAreCountedAndRequeuedPromptly) {
    reach_streaming();

    device.queue_frame(FrameDescriptor{/*buffer_index=*/2, /*bytes_used=*/1500, {}});
    machine.step(10ms);

    EXPECT_EQ(stats.frames_received.load(), 1u);
    EXPECT_EQ(stats.bytes_received.load(), 1500u);
    EXPECT_EQ(device.requeue_call_count(), 1);
    EXPECT_TRUE(device.outstanding_buffers().empty());  // nothing left pinned

    clock.advance(100ms);
    device.queue_frame(FrameDescriptor{/*buffer_index=*/1, /*bytes_used=*/1600, {}});
    machine.step(10ms);

    EXPECT_EQ(stats.frames_received.load(), 2u);
    EXPECT_EQ(stats.bytes_received.load(), 3100u);
    EXPECT_EQ(stats.frame_interval_seconds.snapshot().count, 1u);  // no interval on the very first frame
}

// --- device lost mid-run returns to device_absent, not an error state ---

TEST_F(CaptureStateMachineTest, DeviceLostMidStreamReturnsToDeviceAbsent) {
    reach_streaming();

    device.queue_dequeue_result(DequeueResult::DeviceLost);
    machine.step(10ms);

    EXPECT_EQ(machine.state(), CaptureState::DeviceAbsent);
    EXPECT_EQ(stats.error_count(ErrorReason::DeviceLost), 1u);
    EXPECT_EQ(device.close_call_count(), 1);
    EXPECT_EQ(count_events(EventType::DeviceLost), 1u);
    EXPECT_EQ(stats.capture_state.load(), static_cast<int>(CaptureState::DeviceAbsent));
}

// --- reconnects_total: reconnecting is distinct from first start ---

TEST_F(CaptureStateMachineTest, ReconnectingAfterDeviceLossIncrementsReconnectsNotStreamingStarted) {
    reach_streaming();

    device.queue_dequeue_result(DequeueResult::DeviceLost);
    machine.step(10ms);
    ASSERT_EQ(machine.state(), CaptureState::DeviceAbsent);

    auto outcome = machine.step(10ms);  // immediate retry after loss
    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);  // AwaitingProducer -> Streaming
    clock.advance(outcome.recommended_wait);

    ASSERT_EQ(machine.state(), CaptureState::Streaming);
    EXPECT_EQ(stats.reconnects.load(), 1u);
    EXPECT_EQ(count_events(EventType::Reconnecting), 1u);
    EXPECT_EQ(count_events(EventType::StreamingStarted), 1u);  // still just the original
}

// --- stall detection, without exiting ---

TEST_F(CaptureStateMachineTest, StallDetectedAfterThresholdThenClearsOnNextFrame) {
    reach_streaming();

    device.queue_frame(FrameDescriptor{0, 100, {}});
    machine.step(10ms);  // establishes have_last_frame_

    device.set_default_dequeue_result(DequeueResult::Timeout);

    clock.advance(299ms);
    machine.step(10ms);
    EXPECT_EQ(machine.state(), CaptureState::Streaming);  // not yet at threshold
    EXPECT_EQ(stats.stalls.load(), 0u);

    clock.advance(2ms);  // total 301 ms >= 300 ms stall threshold
    machine.step(10ms);
    EXPECT_EQ(machine.state(), CaptureState::Stalled);
    EXPECT_EQ(stats.stalls.load(), 1u);
    EXPECT_EQ(stats.error_count(ErrorReason::Timeout), 1u);
    EXPECT_EQ(count_events(EventType::StallDetected), 1u);

    // Staying stalled must not keep incrementing the counter.
    clock.advance(50ms);
    machine.step(10ms);
    EXPECT_EQ(stats.stalls.load(), 1u);

    device.queue_frame(FrameDescriptor{0, 200, {}});
    machine.step(10ms);
    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    EXPECT_EQ(count_events(EventType::StallCleared), 1u);
    EXPECT_EQ(stats.frames_received.load(), 2u);
}

// --- format change mid-stream is detected, logged, not fatal ---

TEST_F(CaptureStateMachineTest, FormatChangeMidStreamReconfiguresWithoutCountingAsReconnect) {
    const auto original = test_format(1280);
    reach_streaming(original);

    device.queue_frame(FrameDescriptor{0, 100, {}});
    machine.step(10ms);

    const auto changed = test_format(640);
    device.set_format(changed);
    clock.advance(1100ms);  // past the periodic format re-check interval

    device.queue_frame(FrameDescriptor{0, 100, {}});
    machine.step(10ms);

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    ASSERT_EQ(count_events(EventType::FormatChanged), 1u);
    EXPECT_TRUE(events.events.back().type == EventType::FormatChanged);
    EXPECT_TRUE(events.events.back().format == changed);
    EXPECT_TRUE(events.events.back().previous_format == original);

    StreamFormat published{};
    ASSERT_TRUE(stats.stream_info(published));
    EXPECT_TRUE(published == changed);

    // Not a reconnection: the device never left.
    EXPECT_EQ(stats.reconnects.load(), 0u);
    EXPECT_EQ(count_events(EventType::Reconnecting), 0u);
    EXPECT_EQ(count_events(EventType::StreamingStarted), 1u);
}

// --- dequeue errors are counted but do not knock the daemon out of streaming ---

TEST_F(CaptureStateMachineTest, DequeueErrorIncrementsCounterAndStaysStreaming) {
    reach_streaming();

    device.queue_dequeue_result(DequeueResult::Error);
    machine.step(10ms);

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    EXPECT_EQ(stats.error_count(ErrorReason::Dequeue), 1u);
}

// --- buffer setup failures retry from awaiting_producer rather than crashing ---

TEST_F(CaptureStateMachineTest, BufferErrorDuringConfiguringRetriesFromAwaitingProducer) {
    device.set_open_result(OpenResult::Opened);
    device.set_format_result(FormatResult::Known);
    device.set_format(test_format());
    device.set_start_result(StreamStartResult::BufferError);

    auto outcome = machine.step(10ms);  // -> AwaitingProducer
    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);  // format known, start_streaming fails

    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
    EXPECT_EQ(stats.error_count(ErrorReason::Buffer), 1u);
    EXPECT_GT(outcome.recommended_wait, 0ms);
}

// --- a genuine format-query failure is counted separately from "no producer" ---

TEST_F(CaptureStateMachineTest, FormatQueryErrorIsCountedAndRetried) {
    device.set_open_result(OpenResult::Opened);
    device.set_format_result(FormatResult::Error);

    auto outcome = machine.step(10ms);  // -> AwaitingProducer
    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);  // format query errors out

    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
    EXPECT_EQ(stats.error_count(ErrorReason::Format), 1u);
    EXPECT_GT(outcome.recommended_wait, 0ms);
}
