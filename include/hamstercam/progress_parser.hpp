// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

#include "hamstercam/clock.hpp"
#include "hamstercam/producer_stats.hpp"

namespace hamstercam {

// Parses ffmpeg's `-progress` key=value text into ProducerStats. No socket
// I/O: feed() takes bytes already read.
//
// Malformed input is skipped a key at a time, never a block at a time --
// ffmpeg emits keys this exporter ignores, and one bad field must not blank
// out the good ones beside it.
class ProgressParser {
public:
    ProgressParser(ProducerStats& stats, Clock& clock) : stats_(stats), clock_(clock) {}

    // Consumes a chunk of bytes read from the progress socket. May be called
    // with a partial line at either end; buffering carries across calls.
    void feed(std::string_view chunk);

private:
    void parse_line(std::string_view line);

    ProducerStats& stats_;
    Clock& clock_;
    std::string buffer_;
};

}  // namespace hamstercam
