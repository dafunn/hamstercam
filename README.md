# hamstercam

hamstercam watches a hamster and reports when he is out and about. 
This is intended to run on a Raspberry Pi 3B+ so it's designed to be
one small binary, no runtime dependencies, and memory usage that stays
flat over time.

Currently it reads frames and reports whether they are arriving;
detection is still work in progress.

It works by having a producer write frames into a v4l2loopback device that
multiple consumers can consume, and the daemon hamstercamd reads those frames
to monitor the hamster enclosure.

    [camera] -> [ffmpeg] -+-> /dev/videoN (raw)  -> hamstercamd
                          +-> /dev/videoM (JPEG) -> a streaming server

## What it reports

Events are JSON lines. A producer restarting at a different resolution looks
like this, and none of it interrupts the daemon:

    {"ts":"...","level":"warn","event":"stall_detected"}
    {"ts":"...","level":"info","event":"stall_cleared"}
    {"ts":"...","level":"info","event":"format_changed","width":640,
     "height":480,"previous_width":320,"previous_height":240}

Metrics are Prometheus text on :9843/metrics:

    hamstercam_capture_state 3
    hamstercam_frames_received_total 6042
    hamstercam_frame_interval_seconds_bucket{le="0.0667"} 5931
    hamstercam_stream_info{width="640",height="480",pixel_format="YU12"} 1

## Run it

    sudo modprobe v4l2loopback video_nr=40 card_label=hc-raw
    ffmpeg -f v4l2 -i /dev/video0 -fps_mode passthrough \
      -f v4l2 -pix_fmt yuv420p /dev/video40 &
    ./build/hamstercamd --device /dev/video40
    curl -s localhost:9843/metrics | grep frames_received

## Build

Needs CMake 3.20+, a C++20 compiler, and network access on first configure.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j"$(nproc)"
    ctest --test-dir build --output-on-failure

Tests need no camera. Options: HAMSTERCAM_BUILD_TESTS (ON),
HAMSTERCAM_SANITIZE (OFF), HAMSTERCAM_WERROR (OFF). MIT; see LICENSE.
