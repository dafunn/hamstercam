// SPDX-License-Identifier: MIT
#include "hamstercam/metrics.hpp"

#include <gtest/gtest.h>

using namespace hamstercam;

TEST(MetricsTest, RendersCountersAndGauges) {
    CaptureStats stats;
    stats.frames_received.store(42);
    stats.bytes_received.store(1234);
    stats.capture_state.store(static_cast<int>(CaptureState::Streaming));

    const auto body = render_metrics(stats, BuildInfo{"1.2.3", "abc123"}, 5.5, 1048576);

    EXPECT_NE(body.find("hamstercam_frames_received_total 42"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_bytes_received_total 1234"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_capture_state 3"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_build_info{version=\"1.2.3\",git_sha=\"abc123\"} 1"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_process_resident_bytes 1048576"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_uptime_seconds 5.5"), std::string::npos);
}

TEST(MetricsTest, EmitsErrorReasonForEveryFixedReason) {
    CaptureStats stats;
    stats.record_error(ErrorReason::Timeout);

    const auto body = render_metrics(stats, BuildInfo{"1.0.0", "deadbee"}, 0.0, 0);

    EXPECT_NE(body.find("hamstercam_capture_errors_total{reason=\"timeout\"} 1"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_capture_errors_total{reason=\"open\"} 0"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_capture_errors_total{reason=\"device_lost\"} 0"), std::string::npos);
}

TEST(MetricsTest, StreamInfoOmittedUntilFormatObserved) {
    CaptureStats stats;
    const auto before = render_metrics(stats, BuildInfo{"1.0.0", "deadbee"}, 0.0, 0);
    EXPECT_EQ(before.find("hamstercam_stream_info"), std::string::npos);

    StreamFormat format;
    format.width = 1280;
    format.height = 720;
    format.pixel_format_fourcc = 0x47504A4D;  // 'MJPG'
    stats.set_stream_info(format);

    const auto after = render_metrics(stats, BuildInfo{"1.0.0", "deadbee"}, 0.0, 0);
    EXPECT_NE(after.find("hamstercam_stream_info{width=\"1280\",height=\"720\",pixel_format=\"MJPG\"} 1"),
              std::string::npos);
}

TEST(MetricsTest, HistogramBucketsAreCumulative) {
    CaptureStats stats;
    stats.frame_interval_seconds.observe(0.02);
    stats.frame_interval_seconds.observe(0.2);

    const auto body = render_metrics(stats, BuildInfo{"1.0.0", "deadbee"}, 0.0, 0);

    EXPECT_NE(body.find("hamstercam_frame_interval_seconds_bucket{le=\"0.02\"} 1"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_frame_interval_seconds_bucket{le=\"0.25\"} 2"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_frame_interval_seconds_bucket{le=\"+Inf\"} 2"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_frame_interval_seconds_count 2"), std::string::npos);
}
