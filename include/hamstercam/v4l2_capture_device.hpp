// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "hamstercam/capture_device.hpp"

namespace hamstercam {

class V4L2CaptureDevice final : public ICaptureDevice {
public:
    explicit V4L2CaptureDevice(std::string device_path);
    ~V4L2CaptureDevice() override;

    OpenResult open() override;
    void close() noexcept override;
    FormatResult query_format(StreamFormat& out) override;
    StreamStartResult start_streaming(std::uint32_t buffer_count) override;
    void stop_streaming() noexcept override;
    DequeueResult dequeue_frame(std::chrono::milliseconds timeout, FrameDescriptor& out) override;
    void requeue_frame(std::uint32_t buffer_index) override;

private:
    struct MappedBuffer {
        void* address = nullptr;
        std::size_t length = 0;
    };

    void unmap_buffers() noexcept;

    // Whether the driver's VIDIOC_ENUM_FMT lists `fourcc` with
    // V4L2_FMT_FLAG_COMPRESSED set. Returns true (fail toward compressed) if
    // enumeration fails or never lists the fourcc that was just negotiated.
    bool query_compressed(std::uint32_t fourcc) const;

    std::string device_path_;
    int fd_ = -1;
    std::vector<MappedBuffer> buffers_;
    bool streaming_ = false;
};

}  // namespace hamstercam
