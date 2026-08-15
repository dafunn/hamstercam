// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>

namespace hamstercam {

// Injected everywhere the state machine needs "now", so tests can drive
// stall detection and backoff deterministically without sleeping.
class Clock {
public:
    virtual ~Clock() = default;

    virtual std::chrono::steady_clock::time_point steady_now() const = 0;
    virtual std::chrono::system_clock::time_point wall_now() const = 0;
};

class SteadyClock final : public Clock {
public:
    std::chrono::steady_clock::time_point steady_now() const override {
        return std::chrono::steady_clock::now();
    }

    std::chrono::system_clock::time_point wall_now() const override {
        return std::chrono::system_clock::now();
    }
};

}  // namespace hamstercam
