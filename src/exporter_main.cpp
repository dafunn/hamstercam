// SPDX-License-Identifier: MIT
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "hamstercam/clock.hpp"
#include "hamstercam/exporter_metrics.hpp"
#include "hamstercam/http_server.hpp"
#include "hamstercam/json_logger.hpp"
#include "hamstercam/producer_stats.hpp"
#include "hamstercam/progress_parser.hpp"
#include "hamstercam/progress_socket_server.hpp"

using namespace std::chrono_literals;
using namespace hamstercam;

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

const std::set<std::string>& known_flags() {
    static const std::set<std::string> flags = {"--progress-socket", "--metrics-addr", "--log-level"};
    return flags;
}

void print_usage(std::ostream& out) {
    out << "hamstercam-exporter -- exposes an ffmpeg producer's progress as Prometheus metrics.\n"
           "\n"
           "ffmpeg writes progress to a unix socket; this listens on that socket and serves\n"
           "what it reports over HTTP. Point ffmpeg at it with -progress unix://<path>.\n"
           "\n"
           "  --progress-socket PATH   socket to listen on\n"
           "                           (default /run/hamstercam/ffmpeg-progress.sock)\n"
           "  --metrics-addr HOST:PORT where to serve /metrics (default 127.0.0.1:9844)\n"
           "  --log-level LEVEL        debug, info, warn, or error (default info)\n"
           "  --help                   this text\n"
           "\n"
           "Every flag can also be set by environment variable:\n"
           "HAMSTERCAM_PROGRESS_SOCKET, HAMSTERCAM_METRICS_ADDR, HAMSTERCAM_LOG_LEVEL.\n"
           "A flag wins over the matching variable.\n";
}

bool parse_flags(int argc, char** argv, std::map<std::string, std::string>& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string name = arg;
        std::string value;
        bool has_value = false;

        if (const auto eq = arg.find('='); eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_value = true;
        }

        if (known_flags().find(name) == known_flags().end()) {
            std::cerr << "unknown flag: " << name << "\n\n";
            print_usage(std::cerr);
            return false;
        }
        if (!has_value) {
            if (i + 1 >= argc) {
                std::cerr << "flag " << name << " requires a value\n";
                return false;
            }
            value = argv[++i];
        }
        out[name] = value;
    }
    return true;
}

std::string resolve(const std::map<std::string, std::string>& flags, const std::string& flag_name,
                     const char* env_name, const std::string& default_value) {
    if (const auto it = flags.find(flag_name); it != flags.end()) return it->second;
    if (const char* env = std::getenv(env_name); env != nullptr) return env;
    return default_value;
}

}  // namespace

int main(int argc, char** argv) {
    // Checked before the parser, so --help works even alongside a bad flag.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        }
    }

    std::map<std::string, std::string> flags;
    if (!parse_flags(argc, argv, flags)) return 1;

    const std::string socket_path = resolve(flags, "--progress-socket", "HAMSTERCAM_PROGRESS_SOCKET",
                                              "/run/hamstercam/ffmpeg-progress.sock");
    const std::string metrics_addr = resolve(flags, "--metrics-addr", "HAMSTERCAM_METRICS_ADDR", "127.0.0.1:9844");
    const std::string log_level_str = resolve(flags, "--log-level", "HAMSTERCAM_LOG_LEVEL", "info");

    LogLevel log_level;
    if (!parse_log_level(log_level_str, log_level)) {
        std::cerr << "invalid --log-level: " << log_level_str << "\n";
        return 1;
    }

    JsonLogger logger(std::cout, log_level);
    std::move(logger.log(LogLevel::Info, "startup"))
        .field("progress_socket", socket_path)
        .field("metrics_addr", metrics_addr);

    SteadyClock clock;
    ProducerStats stats;
    ProgressParser parser(stats, clock);
    ProgressSocketServer progress_server(socket_path, parser, stats);

    if (!progress_server.start()) {
        std::cerr << "cannot listen on progress socket " << socket_path << "\n"
                  << "  " << progress_server.last_error() << "\n";
        return 1;
    }

    HttpServer http_server(metrics_addr, [&](std::string_view method, std::string_view path) -> HttpResponse {
        if (method != "GET") return HttpResponse{404, "text/plain", "not found\n"};
        if (path == "/metrics") {
            return HttpResponse{200, "text/plain; version=0.0.4; charset=utf-8", render_exporter_metrics(stats)};
        }
        return HttpResponse{404, "text/plain", "not found\n"};
    });

    if (!http_server.start()) {
        std::cerr << "failed to bind metrics address: " << metrics_addr << "\n";
        return 1;
    }

    install_signal_handlers();
    std::thread progress_thread([&] { progress_server.run(); });
    std::thread http_thread([&] { http_server.run(); });

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(200ms);
    }

    progress_server.stop();
    http_server.stop();
    progress_thread.join();
    http_thread.join();

    logger.log(LogLevel::Info, "shutdown");
    return 0;
}
