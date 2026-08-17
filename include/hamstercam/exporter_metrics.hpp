// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "hamstercam/producer_stats.hpp"

namespace hamstercam {

// Prometheus text exposition, format version 0.0.4, for the exporter's own
// metric set. hamstercamd's equivalent is metrics.hpp; both are built on
// metrics_primitives.hpp.
std::string render_exporter_metrics(const ProducerStats& stats);

}  // namespace hamstercam
