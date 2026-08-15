// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace hamstercam {

struct HttpResponse {
    int status = 404;
    std::string content_type = "text/plain";
    std::string body;
};

using HttpHandler = std::function<HttpResponse(std::string_view method, std::string_view path)>;

// Single-threaded accept loop: binds `address` (host:port), then run() blocks
// handling one request at a time until stop() is called from another thread.
class HttpServer {
public:
    HttpServer(std::string address, HttpHandler handler);
    ~HttpServer();

    bool start();
    void run();
    void stop();

private:
    std::string address_;
    HttpHandler handler_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace hamstercam
