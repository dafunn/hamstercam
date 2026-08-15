// SPDX-License-Identifier: MIT
#include "hamstercam/http_server.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <sstream>

namespace hamstercam {

namespace {

bool split_host_port(const std::string& address, std::string& host, std::string& port) {
    const auto colon = address.rfind(':');
    if (colon == std::string::npos) return false;
    host = address.substr(0, colon);
    port = address.substr(colon + 1);
    return true;
}

// Reads and discards the request up to the blank line, keeping only the
// request line: this server never inspects headers or bodies.
bool read_request_line(int fd, std::string& method, std::string& path) {
    std::string buf;
    std::array<char, 512> chunk{};
    while (buf.find("\r\n") == std::string::npos && buf.size() < 8192) {
        const auto n = recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;
        buf.append(chunk.data(), static_cast<std::size_t>(n));
    }

    const auto line_end = buf.find("\r\n");
    const std::string line = line_end == std::string::npos ? buf : buf.substr(0, line_end);

    std::istringstream iss(line);
    std::string version;
    if (!(iss >> method >> path >> version)) return false;
    return true;
}

std::string status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 404:
            return "Not Found";
        default:
            return "Error";
    }
}

void write_response(int fd, const HttpResponse& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << status_text(resp.status) << "\r\n";
    out << "Content-Type: " << resp.content_type << "\r\n";
    out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << resp.body;

    const auto s = out.str();
    std::size_t sent = 0;
    while (sent < s.size()) {
        const auto n = send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

}  // namespace

HttpServer::HttpServer(std::string address, HttpHandler handler)
    : address_(std::move(address)), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool HttpServer::start() {
    std::string host, port;
    if (!split_host_port(address_, host, port)) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return false;

    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        listen_fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd_ < 0) continue;

        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listen_fd_, rp->ai_addr, rp->ai_addrlen) == 0 && listen(listen_fd_, 16) == 0) {
            freeaddrinfo(result);
            return true;
        }
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    freeaddrinfo(result);
    return false;
}

void HttpServer::run() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int rc = poll(&pfd, 1, 200);
        if (rc <= 0) continue;

        const int conn = accept(listen_fd_, nullptr, nullptr);
        if (conn < 0) continue;

        std::string method, path;
        if (read_request_line(conn, method, path)) {
            const HttpResponse resp = handler_(method, path);
            write_response(conn, resp);
        }
        ::close(conn);
    }
}

void HttpServer::stop() { stop_requested_.store(true, std::memory_order_relaxed); }

}  // namespace hamstercam
