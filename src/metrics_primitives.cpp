// SPDX-License-Identifier: MIT
#include "hamstercam/metrics_primitives.hpp"

namespace hamstercam {

std::string format_metric_double(double v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

void write_metric_header(std::ostringstream& out, std::string_view name, std::string_view help,
                          std::string_view type) {
    out << "# HELP " << name << " " << help << "\n";
    out << "# TYPE " << name << " " << type << "\n";
}

void write_metric_sample(std::ostringstream& out, std::string_view name, std::string_view value,
                          const std::vector<MetricLabel>& labels) {
    out << name;
    if (!labels.empty()) {
        out << "{";
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (i > 0) out << ",";
            out << labels[i].name << "=\"" << labels[i].value << "\"";
        }
        out << "}";
    }
    out << " " << value << "\n";
}

void write_counter(std::ostringstream& out, std::string_view name, std::string_view help, std::uint64_t value) {
    write_metric_header(out, name, help, "counter");
    write_metric_sample(out, name, std::to_string(value));
}

void write_gauge(std::ostringstream& out, std::string_view name, std::string_view help, std::string_view value) {
    write_metric_header(out, name, help, "gauge");
    write_metric_sample(out, name, value);
}

}  // namespace hamstercam
