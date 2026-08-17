// SPDX-License-Identifier: MIT
#include "hamstercam/metrics.hpp"

#include <sstream>

#include "hamstercam/capture_state.hpp"
#include "hamstercam/error_reason.hpp"
#include "hamstercam/metrics_primitives.hpp"

namespace hamstercam {

namespace {

void write_histogram(std::ostringstream& out, std::string_view name, std::string_view help,
                      const Histogram& hist) {
    const auto snap = hist.snapshot();
    const auto& bounds = hist.upper_bounds();

    out << "# HELP " << name << " " << help << "\n";
    out << "# TYPE " << name << " histogram\n";

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        cumulative += snap.bucket_counts[i];
        out << name << "_bucket{le=\"" << format_metric_double(bounds[i]) << "\"} " << cumulative << "\n";
    }
    cumulative += snap.bucket_counts.back();
    out << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
    out << name << "_sum " << snap.sum << "\n";
    out << name << "_count " << snap.count << "\n";
}

}  // namespace

std::string render_metrics(const CaptureStats& stats, const BuildInfo& build, double uptime_seconds,
                            std::uint64_t resident_bytes) {
    std::ostringstream out;

    write_counter(out, "hamstercam_frames_received_total", "Frames dequeued.",
                  stats.frames_received.load(std::memory_order_relaxed));
    write_counter(out, "hamstercam_bytes_received_total", "Payload bytes across received frames.",
                  stats.bytes_received.load(std::memory_order_relaxed));
    write_histogram(out, "hamstercam_frame_interval_seconds", "Observed inter-frame gap.",
                     stats.frame_interval_seconds);
    write_gauge(out, "hamstercam_capture_state", "Capture state machine state.",
                std::to_string(stats.capture_state.load(std::memory_order_relaxed)));

    write_metric_header(out, "hamstercam_capture_errors_total", "Capture errors by reason.", "counter");
    for (std::size_t i = 0; i < kErrorReasonCount; ++i) {
        const auto reason = static_cast<ErrorReason>(i);
        write_metric_sample(out, "hamstercam_capture_errors_total", std::to_string(stats.error_count(reason)),
                             {{"reason", std::string(to_string(reason))}});
    }

    write_counter(out, "hamstercam_reconnects_total", "Successful reconnections.",
                  stats.reconnects.load(std::memory_order_relaxed));
    write_counter(out, "hamstercam_stalls_total", "Stall events entered.",
                  stats.stalls.load(std::memory_order_relaxed));
    write_gauge(out, "hamstercam_last_frame_unixtime_seconds", "Wall-clock time of the last frame.",
                std::to_string(stats.last_frame_unixtime.load(std::memory_order_relaxed)));

    StreamFormat format{};
    if (stats.stream_info(format)) {
        write_metric_header(out, "hamstercam_stream_info", "Observed stream format.", "gauge");
        write_metric_sample(out, "hamstercam_stream_info", "1",
                             {{"width", std::to_string(format.width)},
                              {"height", std::to_string(format.height)},
                              {"pixel_format", format.pixel_format_string()}});
    }

    write_metric_header(out, "hamstercam_build_info", "Build version and git SHA.", "gauge");
    write_metric_sample(out, "hamstercam_build_info", "1", {{"version", build.version}, {"git_sha", build.git_sha}});

    write_gauge(out, "hamstercam_process_resident_bytes", "Resident memory, for the flatness check.",
                std::to_string(resident_bytes));
    write_gauge(out, "hamstercam_uptime_seconds", "Seconds since process start.",
                format_metric_double(uptime_seconds));

    return out.str();
}

}  // namespace hamstercam
