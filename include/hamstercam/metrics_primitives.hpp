// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hamstercam {

// One label={value} pair for a Prometheus sample line. `value` is owned, not
// a string_view: callers commonly build it on the fly (std::to_string(width),
// a formatted double), and the primitive must outlive that temporary.
struct MetricLabel {
    std::string_view name;
    std::string value;
};

// Formats a double the way every gauge in this codebase already expects:
// plain ostream formatting, no fixed precision.
std::string format_metric_double(double v);

// "# HELP name help\n# TYPE name type\n" -- the header lines shared by every
// metric, labeled or not, single- or multi-sample.
void write_metric_header(std::ostringstream& out, std::string_view name, std::string_view help,
                          std::string_view type);

// One sample line: `name{label="value",...} value\n`, or `name value\n` when
// labels is empty. Does not write HELP/TYPE -- call write_metric_header once
// per metric name before one or more samples.
void write_metric_sample(std::ostringstream& out, std::string_view name, std::string_view value,
                          const std::vector<MetricLabel>& labels = {});

// HELP + TYPE + a single unlabeled sample: the common case of a metric with
// exactly one time series.
void write_counter(std::ostringstream& out, std::string_view name, std::string_view help, std::uint64_t value);
void write_gauge(std::ostringstream& out, std::string_view name, std::string_view help, std::string_view value);

}  // namespace hamstercam
