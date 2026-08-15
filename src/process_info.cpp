// SPDX-License-Identifier: MIT
#include "hamstercam/process_info.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace hamstercam {

std::uint64_t resident_set_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") != 0) continue;
        std::istringstream iss(line.substr(6));
        std::uint64_t kib = 0;
        iss >> kib;
        return kib * 1024;
    }
    return 0;
}

}  // namespace hamstercam
