# hamstercam

hamstercam watches a hamster and reports when he is out and about.

Right now it is only the plumbing. Working out what the hamster is doing
comes later; today hamstercamd reads frames and tracks whether they arrive.

A producer writes frames into a v4l2loopback device so consumers can read
from it, and hamstercamd reads those frames to monitor the hamster enclosure.

    [camera] -> [ffmpeg] -+-> /dev/videoN (raw)  -> hamstercamd
                          +-> /dev/videoM (JPEG) -> a streaming server

## Status

The state machine, backoff, frame-interval histogram, metric accounting and
V4L2 backend are implemented, and unit tested against a scriptable fake so
the suite needs no camera. The daemon entry point, metrics endpoint and
logging are implemented too. Detection is work in progress.

## Build

Needs CMake 3.20+, a C++20 compiler, and network access on first configure.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j"$(nproc)"
    ctest --test-dir build --output-on-failure

Options: HAMSTERCAM_BUILD_TESTS (ON), HAMSTERCAM_SANITIZE (OFF),
HAMSTERCAM_WERROR (OFF). Licensed under MIT; see LICENSE.
