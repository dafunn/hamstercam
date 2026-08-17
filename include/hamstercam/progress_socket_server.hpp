// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <string>

#include "hamstercam/producer_stats.hpp"
#include "hamstercam/progress_parser.hpp"

namespace hamstercam {

// Listens on a Unix domain socket for ffmpeg's `-progress unix://<path>`
// connection and feeds the bytes to a ProgressParser. Poll-based accept loop,
// run on its own thread so a slow metrics scraper cannot stall this socket
// and backpressure ffmpeg.
//
// One connection at a time: there is one ffmpeg per topology, and a restart
// closes the old connection before the new one dials in.
class ProgressSocketServer {
public:
    ProgressSocketServer(std::string socket_path, ProgressParser& parser, ProducerStats& stats);
    ~ProgressSocketServer();

    bool start();
    void run();
    void stop();

    // Why start() failed. Empty until it returns false.
    const std::string& last_error() const { return last_error_; }

private:
    std::string socket_path_;
    ProgressParser& parser_;
    ProducerStats& stats_;
    int listen_fd_ = -1;
    std::string last_error_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace hamstercam
