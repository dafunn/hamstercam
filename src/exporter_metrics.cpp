// SPDX-License-Identifier: MIT
#include "hamstercam/exporter_metrics.hpp"

#include <sstream>

#include "hamstercam/metrics_primitives.hpp"

namespace hamstercam {

std::string render_exporter_metrics(const ProducerStats& stats) {
    std::ostringstream out;

    write_counter(out, "hamstercam_producer_frames_total", "Frames ffmpeg has processed.",
                  stats.frames_total.load(std::memory_order_relaxed));
    write_gauge(out, "hamstercam_producer_fps", "ffmpeg's reported instantaneous fps.",
                format_metric_double(stats.fps.load(std::memory_order_relaxed)));
    write_counter(out, "hamstercam_producer_dropped_frames_total", "ffmpeg's drop_frames.",
                  stats.dropped_frames_total.load(std::memory_order_relaxed));
    write_counter(out, "hamstercam_producer_duplicated_frames_total", "ffmpeg's dup_frames.",
                  stats.duplicated_frames_total.load(std::memory_order_relaxed));
    write_gauge(out, "hamstercam_producer_speed_ratio", "ffmpeg's speed (1.0 = realtime).",
                format_metric_double(stats.speed_ratio.load(std::memory_order_relaxed)));
    write_gauge(out, "hamstercam_producer_connected", "1 while ffmpeg is attached, else 0.",
                stats.connected.load(std::memory_order_relaxed) ? "1" : "0");
    write_gauge(out, "hamstercam_producer_last_update_unixtime_seconds", "Wall-clock time of the last progress block.",
                std::to_string(stats.last_update_unixtime.load(std::memory_order_relaxed)));

    return out.str();
}

}  // namespace hamstercam
