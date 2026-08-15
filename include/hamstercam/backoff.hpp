// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <chrono>

namespace hamstercam {

// Bounded exponential backoff, so that waiting for a device or a writer costs
// approximately no CPU however long it lasts. next() is called only after a
// failed attempt; reset() is called whenever the state machine makes forward
// progress, so a new phase (e.g. device open succeeding, then waiting for a
// producer) always starts its retries at `initial` rather than wherever the
// previous phase's backoff left off.
class Backoff {
public:
    Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds max)
        : initial_(initial), max_(max), current_(initial) {}

    std::chrono::milliseconds next() {
        const auto delay = current_;
        current_ = std::min(max_, current_ * 2);
        return delay;
    }

    void reset() { current_ = initial_; }

private:
    std::chrono::milliseconds initial_;
    std::chrono::milliseconds max_;
    std::chrono::milliseconds current_;
};

}  // namespace hamstercam
