// SPDX-License-Identifier: MIT
#pragma once

#include "hamstercam/clock.hpp"

namespace hamstercam::testing {

class FakeClock final : public Clock {
public:
    std::chrono::steady_clock::time_point steady_now() const override { return steady_; }
    std::chrono::system_clock::time_point wall_now() const override { return wall_; }

    void advance(std::chrono::milliseconds delta) {
        steady_ += delta;
        wall_ += delta;
    }

private:
    std::chrono::steady_clock::time_point steady_{};
    std::chrono::system_clock::time_point wall_{};
};

}  // namespace hamstercam::testing
