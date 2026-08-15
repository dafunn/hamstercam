// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "hamstercam/capture_device.hpp"
#include "hamstercam/capture_state.hpp"
#include "hamstercam/error_reason.hpp"
#include "hamstercam/histogram.hpp"

namespace hamstercam {

// Backing store for the exported metrics. Written by the capture thread and
// read by the metrics HTTP thread, so every field is either atomic or guarded
// by a mutex -- the two threads never coordinate beyond this.
class CaptureStats {
public:
    CaptureStats() : frame_interval_seconds(frame_interval_buckets()) {}

    std::atomic<std::uint64_t> frames_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<int> capture_state{static_cast<int>(CaptureState::DeviceAbsent)};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<std::uint64_t> stalls{0};
    std::atomic<std::int64_t> last_frame_unixtime{0};
    Histogram frame_interval_seconds;

    void record_error(ErrorReason reason) {
        errors_[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t error_count(ErrorReason reason) const {
        return errors_[static_cast<std::size_t>(reason)].load(std::memory_order_relaxed);
    }

    void set_stream_info(const StreamFormat& format) {
        std::lock_guard<std::mutex> lock(stream_info_mutex_);
        stream_info_ = format;
        stream_info_known_ = true;
    }

    // Returns false if no format has been observed yet.
    bool stream_info(StreamFormat& out) const {
        std::lock_guard<std::mutex> lock(stream_info_mutex_);
        if (!stream_info_known_) return false;
        out = stream_info_;
        return true;
    }

private:
    std::atomic<std::uint64_t> errors_[kErrorReasonCount]{};

    mutable std::mutex stream_info_mutex_;
    bool stream_info_known_ = false;
    StreamFormat stream_info_{};
};

}  // namespace hamstercam
