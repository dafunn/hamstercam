// SPDX-License-Identifier: MIT
#include "hamstercam/histogram.hpp"

#include <gtest/gtest.h>

using hamstercam::Histogram;

TEST(HistogramTest, PlacesSamplesInCorrectBucket) {
    Histogram hist({0.1, 0.5, 1.0});

    hist.observe(0.05);  // bucket 0 (<= 0.1)
    hist.observe(0.1);   // bucket 0 (<= 0.1, boundary inclusive)
    hist.observe(0.3);   // bucket 1 (<= 0.5)
    hist.observe(2.0);   // overflow (+Inf)

    const auto snap = hist.snapshot();
    ASSERT_EQ(snap.bucket_counts.size(), 4u);  // 3 finite bounds + overflow
    EXPECT_EQ(snap.bucket_counts[0], 2u);
    EXPECT_EQ(snap.bucket_counts[1], 1u);
    EXPECT_EQ(snap.bucket_counts[2], 0u);
    EXPECT_EQ(snap.bucket_counts[3], 1u);
    EXPECT_EQ(snap.count, 4u);
    EXPECT_DOUBLE_EQ(snap.sum, 0.05 + 0.1 + 0.3 + 2.0);
}

TEST(HistogramTest, EmptyHistogramHasZeroedSnapshot) {
    Histogram hist(hamstercam::frame_interval_buckets());

    const auto snap = hist.snapshot();
    EXPECT_EQ(snap.count, 0u);
    EXPECT_DOUBLE_EQ(snap.sum, 0.0);
    for (const auto count : snap.bucket_counts) {
        EXPECT_EQ(count, 0u);
    }
}
