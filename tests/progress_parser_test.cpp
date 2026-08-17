// SPDX-License-Identifier: MIT
#include "hamstercam/progress_parser.hpp"

#include <gtest/gtest.h>

#include "fake_clock.hpp"

using namespace hamstercam;
using hamstercam::testing::FakeClock;

namespace {

constexpr char kBlock[] =
    "frame=1234\n"
    "fps=15.00\n"
    "bitrate=128.0kbits/s\n"
    "total_size=N/A\n"
    "out_time_us=82266666\n"
    "dup_frames=3\n"
    "drop_frames=2\n"
    "speed=1.02x\n"
    "progress=continue\n";

}  // namespace

TEST(ProgressParserTest, ParsesAFullBlock) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed(kBlock);

    EXPECT_EQ(stats.frames_total.load(), 1234u);
    EXPECT_DOUBLE_EQ(stats.fps.load(), 15.0);
    EXPECT_EQ(stats.dropped_frames_total.load(), 2u);
    EXPECT_EQ(stats.duplicated_frames_total.load(), 3u);
    EXPECT_DOUBLE_EQ(stats.speed_ratio.load(), 1.02);
}

TEST(ProgressParserTest, ProgressLineSetsLastUpdateFromTheClock) {
    ProducerStats stats;
    FakeClock clock;
    clock.advance(std::chrono::milliseconds(1700000000000LL));
    ProgressParser parser(stats, clock);

    EXPECT_EQ(stats.last_update_unixtime.load(), 0);
    parser.feed(kBlock);
    EXPECT_EQ(stats.last_update_unixtime.load(), 1700000000);
}

TEST(ProgressParserTest, CounterResetOnProducerRestartIsVisibleNotSmoothed) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("frame=9000\ndrop_frames=40\ndup_frames=5\nprogress=continue\n");
    ASSERT_EQ(stats.frames_total.load(), 9000u);

    parser.feed("frame=3\ndrop_frames=0\ndup_frames=0\nprogress=continue\n");

    EXPECT_EQ(stats.frames_total.load(), 3u);
    EXPECT_EQ(stats.dropped_frames_total.load(), 0u);
    EXPECT_EQ(stats.duplicated_frames_total.load(), 0u);
}

TEST(ProgressParserTest, TruncatedLinesAcrossMultipleFeedCallsStillParse) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("fra");
    parser.feed("me=555\ndrop_fra");
    parser.feed("mes=1\nprogress=con");
    parser.feed("tinue\n");

    EXPECT_EQ(stats.frames_total.load(), 555u);
    EXPECT_EQ(stats.dropped_frames_total.load(), 1u);
}

TEST(ProgressParserTest, LineWithNoEqualsSignIsSkippedNotFatal) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("this is not key=value shaped, well one of them is\nframe=77\nprogress=continue\n");

    EXPECT_EQ(stats.frames_total.load(), 77u);
}

TEST(ProgressParserTest, UnparseableValueSkipsOnlyThatKey) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("frame=not-a-number\nfps=15.5\nprogress=continue\n");

    EXPECT_EQ(stats.frames_total.load(), 0u);  // corrupted value never applied
    EXPECT_DOUBLE_EQ(stats.fps.load(), 15.5);  // the rest of the block still is
}

TEST(ProgressParserTest, NAValuesDoNotOverwritePreviousReading) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("fps=12.5\nspeed=0.98x\nprogress=continue\n");
    parser.feed("fps=N/A\nspeed=N/A\nprogress=continue\n");

    EXPECT_DOUBLE_EQ(stats.fps.load(), 12.5);
    EXPECT_DOUBLE_EQ(stats.speed_ratio.load(), 0.98);
}

TEST(ProgressParserTest, SpeedTrailingXIsStripped) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("speed=2.5x\nprogress=continue\n");

    EXPECT_DOUBLE_EQ(stats.speed_ratio.load(), 2.5);
}

TEST(ProgressParserTest, ToleratesCrlfLineEndings) {
    ProducerStats stats;
    FakeClock clock;
    ProgressParser parser(stats, clock);

    parser.feed("frame=42\r\nprogress=continue\r\n");

    EXPECT_EQ(stats.frames_total.load(), 42u);
}
