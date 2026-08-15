// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace hamstercam {

// Bucket boundaries clustered around a 15 fps expectation (66.7 ms), so the
// interesting resolution sits either side of the nominal frame interval
// rather than being averaged away.
inline const std::vector<double>& frame_interval_buckets() {
    static const std::vector<double> buckets = {0.005, 0.01, 0.02, 0.033, 0.05, 0.0667,
                                                 0.1,   0.15, 0.25, 0.5,   1.0,  2.0, 5.0};
    return buckets;
}

// Prometheus-style histogram: per-bucket (non-cumulative) counts plus an
// overflow bucket for +Inf, a running sum, and a total count. The exporter
// prefix-sums these into cumulative "le" lines at render time.
class Histogram {
public:
    explicit Histogram(std::vector<double> upper_bounds)
        : bounds_(std::move(upper_bounds)), counts_(bounds_.size() + 1, 0) {}

    void observe(double value_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = bounds_.size();
        for (std::size_t i = 0; i < bounds_.size(); ++i) {
            if (value_seconds <= bounds_[i]) {
                index = i;
                break;
            }
        }
        ++counts_[index];
        sum_ += value_seconds;
        ++total_;
    }

    struct Snapshot {
        std::vector<std::uint64_t> bucket_counts;  // size == upper_bounds().size() + 1
        double sum = 0;
        std::uint64_t count = 0;
    };

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Snapshot{counts_, sum_, total_};
    }

    const std::vector<double>& upper_bounds() const { return bounds_; }

private:
    std::vector<double> bounds_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> counts_;
    double sum_ = 0;
    std::uint64_t total_ = 0;
};

}  // namespace hamstercam
