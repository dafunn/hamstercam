// SPDX-License-Identifier: MIT
#include "hamstercam/progress_parser.hpp"

#include <chrono>

namespace hamstercam {

namespace {

// A line ffmpeg never terminates -- a wedged or misbehaving writer -- must
// not grow this buffer without bound. Mirrors HttpServer's request-line cap
// (http_server.cpp): past this size, treat the pending partial line as
// unrecoverable and drop it rather than accumulate forever.
constexpr std::size_t kMaxBufferedLine = 65536;

bool parse_u64(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

bool parse_double(std::string_view s, double& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        const double v = std::stod(std::string(s), &pos);
        if (pos != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

void ProgressParser::feed(std::string_view chunk) {
    buffer_.append(chunk);
    if (buffer_.size() > kMaxBufferedLine) {
        buffer_.clear();
        return;
    }

    std::size_t consumed = 0;
    for (;;) {
        const auto nl = buffer_.find('\n', consumed);
        if (nl == std::string::npos) break;
        parse_line(std::string_view(buffer_).substr(consumed, nl - consumed));
        consumed = nl + 1;
    }
    buffer_.erase(0, consumed);
}

void ProgressParser::parse_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    const auto eq = line.find('=');
    if (eq == std::string_view::npos) return;  // malformed line -- no key=value shape, skip it

    const std::string_view key = line.substr(0, eq);
    const std::string_view value = line.substr(eq + 1);

    if (key == "frame") {
        std::uint64_t v = 0;
        if (parse_u64(value, v)) stats_.frames_total.store(v, std::memory_order_relaxed);
    } else if (key == "fps") {
        double v = 0;
        if (value != "N/A" && parse_double(value, v)) stats_.fps.store(v, std::memory_order_relaxed);
    } else if (key == "drop_frames") {
        std::uint64_t v = 0;
        if (parse_u64(value, v)) stats_.dropped_frames_total.store(v, std::memory_order_relaxed);
    } else if (key == "dup_frames") {
        std::uint64_t v = 0;
        if (parse_u64(value, v)) stats_.duplicated_frames_total.store(v, std::memory_order_relaxed);
    } else if (key == "speed") {
        std::string_view numeric = value;
        if (!numeric.empty() && numeric.back() == 'x') numeric.remove_suffix(1);
        double v = 0;
        if (numeric != "N/A" && parse_double(numeric, v)) stats_.speed_ratio.store(v, std::memory_order_relaxed);
    } else if (key == "progress") {
        // Ends a block, whether "continue" or "end". Timestamping here rather
        // than per key means the metric tracks whole blocks.
        stats_.last_update_unixtime.store(
            std::chrono::duration_cast<std::chrono::seconds>(clock_.wall_now().time_since_epoch()).count(),
            std::memory_order_relaxed);
    }
}

}  // namespace hamstercam
