// SPDX-License-Identifier: MIT
#include "hamstercam/backoff.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using hamstercam::Backoff;

TEST(BackoffTest, DoublesUpToMax) {
    Backoff backoff(100ms, 800ms);

    EXPECT_EQ(backoff.next(), 100ms);
    EXPECT_EQ(backoff.next(), 200ms);
    EXPECT_EQ(backoff.next(), 400ms);
    EXPECT_EQ(backoff.next(), 800ms);
    EXPECT_EQ(backoff.next(), 800ms);  // capped
}

TEST(BackoffTest, ResetReturnsToInitial) {
    Backoff backoff(100ms, 800ms);

    backoff.next();
    backoff.next();
    backoff.reset();

    EXPECT_EQ(backoff.next(), 100ms);
}
