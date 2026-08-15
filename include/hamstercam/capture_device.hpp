// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hamstercam {

// What VIDIOC_G_FMT reports. Observed, never requested: this reads a virtual
// device whose format is established by whatever process is writing to it.
struct StreamFormat {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_format_fourcc = 0;  // raw V4L2 four-character code

    friend bool operator==(const StreamFormat&, const StreamFormat&) = default;

    // Decodes pixel_format_fourcc into its 4 printable characters, for the
    // hamstercam_stream_info{pixel_format} label. The daemon never interprets
    // the format beyond this string -- it counts frames and bytes without
    // decoding anything, so every pixel layout is handled identically.
    std::string pixel_format_string() const;
};

// Metadata for one dequeued buffer. Deliberately carries no pixel pointer:
// nothing in this daemon reads frame payloads, and not exposing them is what
// keeps that true.
struct FrameDescriptor {
    std::uint32_t buffer_index = 0;
    std::size_t bytes_used = 0;
    std::chrono::steady_clock::time_point captured_at{};
};

enum class OpenResult {
    Opened,
    Absent,  // ENOENT/EACCES or equivalent -- routine, the device may not exist yet
};

enum class FormatResult {
    Known,
    // VIDIOC_G_FMT returned EINVAL: the device exists and opened fine, but
    // nothing is writing to it, so no format has been established. Routine --
    // it is what you get whenever this daemon outlives or outpaces its writer.
    NoProducer,
    Error,  // a genuine query failure
};

enum class StreamStartResult {
    Started,
    BufferError,
};

enum class DequeueResult {
    Frame,
    Timeout,      // poll() timed out; no frame this call
    DeviceLost,   // the device disappeared mid-stream
    Error,        // dequeue failed for some other reason
};

// The narrow seam between the state machine and V4L2. The real implementation
// wraps the ioctls; tests use a fake, which is what lets the rest of the
// daemon be tested on a machine with no video device -- CI containers cannot
// load kernel modules, so no virtual device can exist there.
//
// Every method reports expected operating conditions through its return value,
// never through exceptions. A missing device and an absent producer are this
// daemon's normal working conditions, not exceptional ones.
class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;

    ICaptureDevice() = default;
    ICaptureDevice(const ICaptureDevice&) = delete;
    ICaptureDevice& operator=(const ICaptureDevice&) = delete;

    virtual OpenResult open() = 0;
    virtual void close() noexcept = 0;

    // VIDIOC_G_FMT only, never VIDIOC_S_FMT. The writer owns the format on a
    // virtual device; a reader requesting one is ignored at best and fights
    // the writer at worst.
    virtual FormatResult query_format(StreamFormat& out) = 0;

    // Sizes and maps buffers for the format most recently returned by
    // query_format() on this instance.
    virtual StreamStartResult start_streaming(std::uint32_t buffer_count) = 0;
    virtual void stop_streaming() noexcept = 0;

    // Blocks up to `timeout` waiting for a buffer, then dequeues it.
    virtual DequeueResult dequeue_frame(std::chrono::milliseconds timeout, FrameDescriptor& out) = 0;

    // Returns a buffer to the driver queue. Must be called promptly after
    // every successful dequeue_frame(): an un-requeued buffer stays pinned,
    // and once enough are pinned the driver has nowhere to put new frames.
    virtual void requeue_frame(std::uint32_t buffer_index) = 0;
};

}  // namespace hamstercam
