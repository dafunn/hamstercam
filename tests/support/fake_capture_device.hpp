// SPDX-License-Identifier: MIT
#pragma once

#include <deque>
#include <set>
#include <utility>

#include "hamstercam/capture_device.hpp"
#include "hamstercam/clock.hpp"

namespace hamstercam::testing {

// A scriptable ICaptureDevice for driving the state machine through every
// scenario without hardware: device absent, present with no producer, normal
// streaming, format change mid-stream, device loss mid-stream, stalls, and
// dequeue errors. Several of these are difficult or slow to reproduce against
// a real device and impossible in a CI container, which cannot load the
// kernel module a virtual video device needs.
class FakeCaptureDevice final : public ICaptureDevice {
public:
    // Auto-stamps dequeued frames with the clock's current time, so tests
    // queue frames without having to hand-compute captured_at.
    explicit FakeCaptureDevice(const Clock& clock) : clock_(clock) {}

    // --- test control surface ---
    void set_open_result(OpenResult result) { open_result_ = result; }
    void set_format_result(FormatResult result) { format_result_ = result; }
    void set_format(const StreamFormat& format) { format_ = format; }
    void set_start_result(StreamStartResult result) { start_result_ = result; }
    void set_default_dequeue_result(DequeueResult result) { default_dequeue_result_ = result; }

    void queue_frame(FrameDescriptor frame) { dequeue_queue_.emplace_back(DequeueResult::Frame, frame); }
    void queue_dequeue_result(DequeueResult result) { dequeue_queue_.emplace_back(result, FrameDescriptor{}); }

    bool is_open() const { return is_open_; }
    bool is_streaming() const { return is_streaming_; }
    int open_call_count() const { return open_call_count_; }
    int close_call_count() const { return close_call_count_; }
    int requeue_call_count() const { return requeue_call_count_; }
    std::uint32_t last_buffer_count_requested() const { return buffer_count_; }

    // Buffers dequeued but not yet requeued. Non-empty after step() returns
    // means a buffer is still pinned; enough of those and the driver runs out
    // of somewhere to put frames.
    const std::set<std::uint32_t>& outstanding_buffers() const { return outstanding_; }

    // --- ICaptureDevice ---
    OpenResult open() override {
        ++open_call_count_;
        is_open_ = (open_result_ == OpenResult::Opened);
        return open_result_;
    }

    void close() noexcept override {
        is_open_ = false;
        is_streaming_ = false;
        ++close_call_count_;
    }

    FormatResult query_format(StreamFormat& out) override {
        if (format_result_ == FormatResult::Known) out = format_;
        return format_result_;
    }

    StreamStartResult start_streaming(std::uint32_t buffer_count) override {
        buffer_count_ = buffer_count;
        is_streaming_ = (start_result_ == StreamStartResult::Started);
        return start_result_;
    }

    void stop_streaming() noexcept override { is_streaming_ = false; }

    DequeueResult dequeue_frame(std::chrono::milliseconds /*timeout*/, FrameDescriptor& out) override {
        if (dequeue_queue_.empty()) return default_dequeue_result_;

        auto [result, frame] = dequeue_queue_.front();
        dequeue_queue_.pop_front();
        if (result == DequeueResult::Frame) {
            frame.captured_at = clock_.steady_now();
            out = frame;
            outstanding_.insert(frame.buffer_index);
        }
        return result;
    }

    void requeue_frame(std::uint32_t buffer_index) override {
        ++requeue_call_count_;
        outstanding_.erase(buffer_index);
    }

private:
    const Clock& clock_;

    OpenResult open_result_ = OpenResult::Absent;
    FormatResult format_result_ = FormatResult::NoProducer;
    StreamFormat format_{};
    StreamStartResult start_result_ = StreamStartResult::Started;
    DequeueResult default_dequeue_result_ = DequeueResult::Timeout;

    std::deque<std::pair<DequeueResult, FrameDescriptor>> dequeue_queue_;
    std::set<std::uint32_t> outstanding_;

    bool is_open_ = false;
    bool is_streaming_ = false;
    std::uint32_t buffer_count_ = 0;

    int open_call_count_ = 0;
    int close_call_count_ = 0;
    int requeue_call_count_ = 0;
};

}  // namespace hamstercam::testing
