// SPDX-License-Identifier: MIT
//
// Needs a real V4L2 device and is never run by CI (see CMakeLists.txt).
// Point HAMSTERCAM_TEST_DEVICE at a v4l2loopback device before running.
#include "hamstercam/v4l2_capture_device.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hamstercam;

namespace {

const char* test_device_path() { return std::getenv("HAMSTERCAM_TEST_DEVICE"); }

}  // namespace

TEST(V4L2CaptureDeviceIntegrationTest, OpeningANonexistentPathIsAbsent) {
    V4L2CaptureDevice device("/dev/hamstercam-does-not-exist");
    EXPECT_EQ(device.open(), OpenResult::Absent);
}

TEST(V4L2CaptureDeviceIntegrationTest, OpensRealDeviceAndQueriesFormat) {
    const char* path = test_device_path();
    if (path == nullptr) GTEST_SKIP() << "set HAMSTERCAM_TEST_DEVICE to a real v4l2loopback device to run this";

    V4L2CaptureDevice device(path);
    ASSERT_EQ(device.open(), OpenResult::Opened);

    StreamFormat format{};
    const auto result = device.query_format(format);
    EXPECT_TRUE(result == FormatResult::Known || result == FormatResult::NoProducer);

    device.close();
}

TEST(V4L2CaptureDeviceIntegrationTest, StreamsFramesWhenProducerPresent) {
    const char* path = test_device_path();
    if (path == nullptr) GTEST_SKIP() << "set HAMSTERCAM_TEST_DEVICE to a real v4l2loopback device to run this";

    V4L2CaptureDevice device(path);
    ASSERT_EQ(device.open(), OpenResult::Opened);

    StreamFormat format{};
    if (device.query_format(format) != FormatResult::Known) {
        GTEST_SKIP() << "no producer attached to " << path;
    }

    ASSERT_EQ(device.start_streaming(4), StreamStartResult::Started);

    FrameDescriptor frame{};
    const auto result = device.dequeue_frame(std::chrono::milliseconds(2000), frame);
    EXPECT_EQ(result, DequeueResult::Frame);
    if (result == DequeueResult::Frame) device.requeue_frame(frame.buffer_index);

    device.stop_streaming();
    device.close();
}
