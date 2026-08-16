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
    config.awaiting_producer_max_backoff = 150ms;
    return config;
}

// Compressed by default, matching the fourcc: MJPEG frames legitimately vary
// in size, so this is the shape most existing tests want when they don't
// care about format-change detection at all.
StreamFormat test_format(std::uint32_t width = 1280) {
    StreamFormat format;
    format.width = width;
    format.height = 720;
    format.pixel_format_fourcc = 0x47504A4D;  // 'MJPG'
    format.size_image = width * 720;  // an upper bound only; irrelevant since compressed
    format.compressed = true;
    return format;
}

// An uncompressed format, where size_image is exact rather than an upper
// bound -- the condition the payload-size check needs to mean anything.
StreamFormat uncompressed_format(std::uint32_t width, std::uint32_t height, std::uint32_t size_image) {
    StreamFormat format;
    format.width = width;
    format.height = height;
    format.pixel_format_fourcc = 0x32315559;  // 'YU12'
    format.size_image = size_image;
    format.compressed = false;
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

// --- a stall now releases the device immediately, not on the next frame ---

TEST_F(CaptureStateMachineTest, StallDetectedReleasesDeviceAndReportsAwaitingProducerNotStalled) {
    reach_streaming();

    device.queue_frame(FrameDescriptor{0, 100, {}});
    machine.step(10ms);  // establishes have_last_frame_

    device.set_default_dequeue_result(DequeueResult::Timeout);
    device.set_format_result(FormatResult::NoProducer);  // nothing there once released

    clock.advance(299ms);
    machine.step(10ms);
    EXPECT_EQ(machine.state(), CaptureState::Streaming);  // not yet at threshold
    EXPECT_EQ(device.close_call_count(), 0);

    clock.advance(2ms);  // total 301 ms >= 300 ms stall threshold
    machine.step(10ms);

    // Released completely, not merely stopped -- asserted through open/close
    // counts, not inferred from state.
    EXPECT_EQ(device.close_call_count(), 1);
    EXPECT_FALSE(device.is_open());
    EXPECT_EQ(stats.stalls.load(), 1u);
    EXPECT_EQ(stats.error_count(ErrorReason::Timeout), 1u);
    EXPECT_EQ(count_events(EventType::StallDetected), 1u);

    // Reported as awaiting_producer throughout, never resting at stalled: a
    // scrape must never be able to distinguish this from a cold start that
    // has not yet seen a producer. stalls_total is the durable signal.
    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
    EXPECT_EQ(stats.capture_state.load(), static_cast<int>(CaptureState::AwaitingProducer));

    // Staying in the poll must not keep incrementing the stall counter.
    clock.advance(50ms);
    machine.step(10ms);
    EXPECT_EQ(stats.stalls.load(), 1u);
    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
}

TEST_F(CaptureStateMachineTest, RecoveryPollsTransientlyAndHoldsNoFdBetweenAttempts) {
    reach_streaming();

    device.queue_frame(FrameDescriptor{0, 100, {}});
    machine.step(10ms);

    device.set_default_dequeue_result(DequeueResult::Timeout);
    device.set_format_result(FormatResult::NoProducer);
    clock.advance(301ms);
    machine.step(10ms);
    ASSERT_EQ(machine.state(), CaptureState::AwaitingProducer);

    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(device.is_open());  // never held between polls
        const auto opens_before = device.open_call_count();
        const auto closes_before = device.close_call_count();

        auto outcome = machine.step(10ms);

        // This attempt itself opened and closed again -- it did not reuse a
        // held-open fd from a previous poll.
        EXPECT_EQ(device.open_call_count(), opens_before + 1);
        EXPECT_EQ(device.close_call_count(), closes_before + 1);
        EXPECT_FALSE(device.is_open());

        clock.advance(outcome.recommended_wait);
    }

    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
}

// --- producer reappearing after a release: same format vs. a different one ---

TEST_F(CaptureStateMachineTest, ProducerReappearingWithSameFormatResumesWithoutFormatChanged) {
    const auto format = uncompressed_format(1280, 720, 1382400);
    reach_streaming(format);

    device.queue_frame(FrameDescriptor{0, 1382400, {}});
    machine.step(10ms);

    device.set_default_dequeue_result(DequeueResult::Timeout);
    clock.advance(301ms);
    machine.step(10ms);
    ASSERT_EQ(machine.state(), CaptureState::AwaitingProducer);

    // The producer never actually left; the next transient probe finds the
    // same format it always had.
    auto outcome = machine.step(10ms);
    clock.advance(outcome.recommended_wait);

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    EXPECT_EQ(count_events(EventType::StallCleared), 1u);
    EXPECT_EQ(count_events(EventType::FormatChanged), 0u);
    EXPECT_EQ(count_events(EventType::StreamingStarted), 2u);  // original start, plus this resumption

    // Not a reconnection: the device was released deliberately, not lost.
    EXPECT_EQ(stats.reconnects.load(), 0u);
    EXPECT_EQ(count_events(EventType::Reconnecting), 0u);
}

TEST_F(CaptureStateMachineTest, ProducerReappearingWithDifferentFormatEmitsFormatChangedAndUpdatesStreamInfo) {
    const auto original = uncompressed_format(1280, 720, 1382400);
    reach_streaming(original);

    device.queue_frame(FrameDescriptor{0, 1382400, {}});
    machine.step(10ms);  // establishes have_last_frame_

    device.set_default_dequeue_result(DequeueResult::Timeout);
    clock.advance(301ms);  // past the 300 ms stall threshold
    machine.step(10ms);
    ASSERT_EQ(machine.state(), CaptureState::AwaitingProducer);

    // A producer restart always passes through a stall -- restarting takes
    // far longer than the stall threshold -- so this is the path a format
    // change actually arrives on in practice.
    const auto changed = uncompressed_format(640, 480, 460800);
    device.set_format(changed);
    auto outcome = machine.step(10ms);
    clock.advance(outcome.recommended_wait);

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    ASSERT_EQ(count_events(EventType::StallCleared), 1u);
    ASSERT_EQ(count_events(EventType::FormatChanged), 1u);

    // Ordering: stall_cleared, then format_changed, then streaming_started.
    ASSERT_GE(events.events.size(), 3u);
    EXPECT_TRUE(events.events[events.events.size() - 3].type == EventType::StallCleared);
    EXPECT_TRUE(events.events[events.events.size() - 2].type == EventType::FormatChanged);
    EXPECT_TRUE(events.events.back().type == EventType::StreamingStarted);
    EXPECT_TRUE(events.events[events.events.size() - 2].format == changed);
    EXPECT_TRUE(events.events[events.events.size() - 2].previous_format == original);

    StreamFormat published{};
    ASSERT_TRUE(stats.stream_info(published));
    EXPECT_TRUE(published == changed);

    EXPECT_EQ(stats.reconnects.load(), 0u);
    EXPECT_EQ(count_events(EventType::Reconnecting), 0u);
}

// --- format change mid-stream, detected from the payload, not an ioctl ---

TEST_F(CaptureStateMachineTest, UncompressedPayloadMismatchMidStreamTriggersReleaseAndCountsGeometryError) {
    const auto original = uncompressed_format(1280, 720, 1382400);
    reach_streaming(original);

    device.queue_frame(FrameDescriptor{0, 1382400, {}});  // matches size_image: no mismatch
    machine.step(10ms);
    EXPECT_EQ(device.close_call_count(), 0);
    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 0u);

    // The producer already restarted at a new resolution without frames ever
    // stopping -- truncation makes the producer write *more* often, not
    // less, so no stall trips here. bytesused disagreeing with the
    // negotiated size_image is the only signal available.
    device.queue_frame(FrameDescriptor{0, 460800, {}});  // no longer matches the negotiated size_image
    machine.step(10ms);

    EXPECT_EQ(device.close_call_count(), 1);
    EXPECT_FALSE(device.is_open());
    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 1u);
    EXPECT_EQ(stats.error_count(ErrorReason::Timeout), 0u);  // not a stall
    EXPECT_EQ(machine.state(), CaptureState::AwaitingProducer);
    EXPECT_EQ(stats.reconnects.load(), 0u);

    // The transient probe now observes the producer's new format.
    const auto changed = uncompressed_format(640, 480, 460800);
    device.set_format(changed);
    auto outcome = machine.step(10ms);
    clock.advance(outcome.recommended_wait);

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    ASSERT_EQ(count_events(EventType::FormatChanged), 1u);
    EXPECT_TRUE(events.events.back().type == EventType::StreamingStarted);
    StreamFormat published{};
    ASSERT_TRUE(stats.stream_info(published));
    EXPECT_TRUE(published == changed);
    EXPECT_EQ(stats.reconnects.load(), 0u);
    EXPECT_EQ(count_events(EventType::Reconnecting), 0u);
}

// --- compressed formats: size_image is only an upper bound, so no per-frame check ---

TEST_F(CaptureStateMachineTest, CompressedStreamWithVaryingFrameSizesAndNoStallTriggersNothing) {
    reach_streaming();  // default test_format(): compressed MJPEG

    // MJPEG frames legitimately vary in size frame to frame; none of this
    // may be mistaken for a format change.
    for (std::uint32_t i = 0; i < 50; ++i) {
        clock.advance(100ms);  // well under the 300 ms stall threshold, matches expected_fps
        device.queue_frame(FrameDescriptor{0, 50000 + i * 137, {}});
        machine.step(10ms);
    }

    EXPECT_EQ(machine.state(), CaptureState::Streaming);
    EXPECT_EQ(count_events(EventType::FormatChanged), 0u);
    EXPECT_EQ(device.close_call_count(), 0);
    EXPECT_EQ(stats.frames_received.load(), 50u);
    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 0u);
}

// --- arrival-rate anomaly is a second, observation-only detector ---

TEST_F(CaptureStateMachineTest, SustainedHighArrivalRateCountsOnceAndDoesNotReleaseDevice) {
    reach_streaming();  // compressed MJPEG: the payload-size check can't fire here

    // expected_fps is 10 (100 ms nominal interval). Deliver well above 5x
    // that -- comfortably past the threshold -- sustained past the 2 s
    // window, without ever approaching the 300 ms stall threshold.
    for (int i = 0; i < 300; ++i) {
        clock.advance(10ms);
        device.queue_frame(FrameDescriptor{0, 50000, {}});
        machine.step(10ms);
    }

    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 1u);  // once per episode, not once per window
    EXPECT_EQ(machine.state(), CaptureState::Streaming);       // detection only -- never released
    EXPECT_EQ(device.close_call_count(), 0);
    EXPECT_EQ(stats.stalls.load(), 0u);
}

TEST_F(CaptureStateMachineTest, ArrivalRateReturningToNormalAllowsANewEpisodeToBeCounted) {
    reach_streaming();

    for (int i = 0; i < 300; ++i) {
        clock.advance(10ms);
        device.queue_frame(FrameDescriptor{0, 50000, {}});
        machine.step(10ms);
    }
    ASSERT_EQ(stats.error_count(ErrorReason::Geometry), 1u);

    // Rate returns to nominal for long enough that a full window resolves
    // entirely within the normal period, clear of any leftover count from
    // the flood: the episode ends. (Long enough matters here -- a window
    // straddling the transition still carries some of the flood's count and
    // can read high on its own.)
    for (int i = 0; i < 80; ++i) {
        clock.advance(100ms);
        device.queue_frame(FrameDescriptor{0, 50000, {}});
        machine.step(10ms);
    }
    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 1u);  // still just the one episode

    // A second flood is a new episode and is counted again.
    for (int i = 0; i < 300; ++i) {
        clock.advance(10ms);
        device.queue_frame(FrameDescriptor{0, 50000, {}});
        machine.step(10ms);
    }
    EXPECT_EQ(stats.error_count(ErrorReason::Geometry), 2u);
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

// --- awaiting_producer and device_absent back off toward separate ceilings ---

TEST_F(CaptureStateMachineTest, AwaitingProducerBackoffCapsAtItsOwnCeilingBelowDeviceAbsents) {
    device.set_open_result(OpenResult::Opened);
    device.set_format_result(FormatResult::NoProducer);

    auto outcome = machine.step(10ms);  // DeviceAbsent -> AwaitingProducer (open succeeds immediately)
    ASSERT_EQ(machine.state(), CaptureState::AwaitingProducer);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);  // first poll for a producer
    EXPECT_EQ(outcome.recommended_wait, 50ms);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    EXPECT_EQ(outcome.recommended_wait, 100ms);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    // Capped at awaiting_producer_max_backoff (150 ms), nowhere near
    // device_absent's separate 400 ms ceiling in this config.
    EXPECT_EQ(outcome.recommended_wait, 150ms);

    clock.advance(outcome.recommended_wait);
    outcome = machine.step(10ms);
    EXPECT_EQ(outcome.recommended_wait, 150ms);  // stays capped
}
