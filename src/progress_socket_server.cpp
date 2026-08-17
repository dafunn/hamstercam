// SPDX-License-Identifier: MIT
#include "hamstercam/progress_socket_server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace hamstercam {

ProgressSocketServer::ProgressSocketServer(std::string socket_path, ProgressParser& parser, ProducerStats& stats)
    : socket_path_(std::move(socket_path)), parser_(parser), stats_(stats) {}

ProgressSocketServer::~ProgressSocketServer() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    ::unlink(socket_path_.c_str());
}

bool ProgressSocketServer::start() {
    const auto fail = [this](std::string what) {
        last_error_ = std::move(what);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        return false;
    };

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return fail(std::string("socket(AF_UNIX): ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        return fail("path is " + std::to_string(socket_path_.size()) + " bytes; the kernel caps a " +
                    "unix socket path at " + std::to_string(sizeof(addr.sun_path) - 1) +
                    ". Use a shorter directory.");
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // A stale socket file left behind by a previous crash must not block
    // bind(): AF_UNIX bind() fails EADDRINUSE if the path already exists,
    // even once nothing is listening on it anymore. Without clearing it,
    // Restart=on-failure could never recover.
    ::unlink(socket_path_.c_str());

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return fail(std::string("bind(): ") + std::strerror(errno));
    }
    if (listen(listen_fd_, 4) != 0) {
        return fail(std::string("listen(): ") + std::strerror(errno));
    }
    last_error_.clear();
    return true;
}

void ProgressSocketServer::run() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        pollfd listen_pfd{listen_fd_, POLLIN, 0};
        if (poll(&listen_pfd, 1, 200) <= 0) continue;

        const int conn = accept(listen_fd_, nullptr, nullptr);
        if (conn < 0) continue;

        stats_.connected.store(true, std::memory_order_relaxed);

        std::array<char, 4096> chunk{};
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            pollfd conn_pfd{conn, POLLIN, 0};
            const int rc = poll(&conn_pfd, 1, 200);
            if (rc < 0) break;
            if (rc == 0) continue;  // no data yet -- ffmpeg paces its own writes

            const auto n = recv(conn, chunk.data(), chunk.size(), 0);
            if (n <= 0) break;  // 0 = orderly close, <0 = error -- either way this connection is done
            parser_.feed(std::string_view(chunk.data(), static_cast<std::size_t>(n)));
        }

        ::close(conn);
        stats_.connected.store(false, std::memory_order_relaxed);
    }
}

void ProgressSocketServer::stop() { stop_requested_.store(true, std::memory_order_relaxed); }

}  // namespace hamstercam
