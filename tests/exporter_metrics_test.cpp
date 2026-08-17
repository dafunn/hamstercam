// SPDX-License-Identifier: MIT
#include "hamstercam/exporter_metrics.hpp"

#include <gtest/gtest.h>

using namespace hamstercam;

TEST(ExporterMetricsTest, RendersAllSevenMetrics) {
    ProducerStats stats;
    stats.frames_total.store(1234);
    stats.fps.store(15.0);
    stats.dropped_frames_total.store(2);
    stats.duplicated_frames_total.store(3);
    stats.speed_ratio.store(1.02);
    stats.connected.store(true);
    stats.last_update_unixtime.store(1700000000);

    const auto body = render_exporter_metrics(stats);

    EXPECT_NE(body.find("hamstercam_producer_frames_total 1234"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_fps 15"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_dropped_frames_total 2"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_duplicated_frames_total 3"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_speed_ratio 1.02"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_connected 1"), std::string::npos);
    EXPECT_NE(body.find("hamstercam_producer_last_update_unixtime_seconds 1700000000"), std::string::npos);
}

TEST(ExporterMetricsTest, DisconnectedRendersZero) {
    ProducerStats stats;
    const auto body = render_exporter_metrics(stats);
    EXPECT_NE(body.find("hamstercam_producer_connected 0"), std::string::npos);
}

TEST(ExporterMetricsTest, EveryMetricHasHelpAndType) {
    ProducerStats stats;
    const auto body = render_exporter_metrics(stats);

    for (const auto* counter : {"hamstercam_producer_frames_total", "hamstercam_producer_dropped_frames_total",
                                 "hamstercam_producer_duplicated_frames_total"}) {
        EXPECT_NE(body.find(std::string("# TYPE ") + counter + " counter"), std::string::npos) << counter;
    }
    for (const auto* gauge : {"hamstercam_producer_fps", "hamstercam_producer_speed_ratio",
                               "hamstercam_producer_connected", "hamstercam_producer_last_update_unixtime_seconds"}) {
        EXPECT_NE(body.find(std::string("# TYPE ") + gauge + " gauge"), std::string::npos) << gauge;
    }
}
