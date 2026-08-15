// SPDX-License-Identifier: MIT
#include "hamstercam/v4l2_capture_device.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hamstercam {

namespace {

int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

}  // namespace

V4L2CaptureDevice::V4L2CaptureDevice(std::string device_path) : device_path_(std::move(device_path)) {}

V4L2CaptureDevice::~V4L2CaptureDevice() {
    stop_streaming();
    close();
}

OpenResult V4L2CaptureDevice::open() {
    fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) return OpenResult::Absent;
    return OpenResult::Opened;
}

void V4L2CaptureDevice::close() noexcept {
    unmap_buffers();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FormatResult V4L2CaptureDevice::query_format(StreamFormat& out) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(fd_, VIDIOC_G_FMT, &fmt) == -1) {
        if (errno == EINVAL) return FormatResult::NoProducer;
        return FormatResult::Error;
    }

    out.width = fmt.fmt.pix.width;
    out.height = fmt.fmt.pix.height;
    out.pixel_format_fourcc = fmt.fmt.pix.pixelformat;
    return FormatResult::Known;
}

StreamStartResult V4L2CaptureDevice::start_streaming(std::uint32_t buffer_count) {
    v4l2_requestbuffers req{};
    req.count = buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &req) == -1) return StreamStartResult::BufferError;

    buffers_.clear();
    buffers_.reserve(req.count);

    for (std::uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1) {
            unmap_buffers();
            return StreamStartResult::BufferError;
        }

        void* addr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                           static_cast<off_t>(buf.m.offset));
        if (addr == MAP_FAILED) {
            unmap_buffers();
            return StreamStartResult::BufferError;
        }
        buffers_.push_back(MappedBuffer{addr, buf.length});

        if (xioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
            unmap_buffers();
            return StreamStartResult::BufferError;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
        unmap_buffers();
        return StreamStartResult::BufferError;
    }

    streaming_ = true;
    return StreamStartResult::Started;
}

void V4L2CaptureDevice::stop_streaming() noexcept {
    if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    unmap_buffers();
}

void V4L2CaptureDevice::unmap_buffers() noexcept {
    for (auto& b : buffers_) {
        if (b.address != nullptr) munmap(b.address, b.length);
    }
    buffers_.clear();
}

DequeueResult V4L2CaptureDevice::dequeue_frame(std::chrono::milliseconds timeout, FrameDescriptor& out) {
    pollfd pfd{fd_, POLLIN, 0};
    const int rc = poll(&pfd, 1, static_cast<int>(timeout.count()));

    if (rc == 0) return DequeueResult::Timeout;
    if (rc < 0) return errno == EINTR ? DequeueResult::Timeout : DequeueResult::Error;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return DequeueResult::DeviceLost;

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) return DequeueResult::Timeout;
        if (errno == ENODEV || errno == EIO) return DequeueResult::DeviceLost;
        return DequeueResult::Error;
    }

    out.buffer_index = buf.index;
    out.bytes_used = buf.bytesused;
    out.captured_at = std::chrono::steady_clock::now();
    return DequeueResult::Frame;
}

void V4L2CaptureDevice::requeue_frame(std::uint32_t buffer_index) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buffer_index;
    xioctl(fd_, VIDIOC_QBUF, &buf);
}

}  // namespace hamstercam
