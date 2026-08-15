// SPDX-License-Identifier: MIT
#include "hamstercam/capture_device.hpp"

namespace hamstercam {

std::string StreamFormat::pixel_format_string() const {
    // V4L2 four-character codes are packed little-endian, one byte per
    // character (see V4L2_FOURCC in videodev2.h).
    std::string out(4, '\0');
    out[0] = static_cast<char>(pixel_format_fourcc & 0xFF);
    out[1] = static_cast<char>((pixel_format_fourcc >> 8) & 0xFF);
    out[2] = static_cast<char>((pixel_format_fourcc >> 16) & 0xFF);
    out[3] = static_cast<char>((pixel_format_fourcc >> 24) & 0xFF);
    return out;
}

}  // namespace hamstercam
