// SPDX-License-Identifier: MIT
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "hamstercam/capture_state_machine.hpp"
#include "hamstercam/clock.hpp"
#include "hamstercam/http_server.hpp"
#include "hamstercam/json_logger.hpp"
#include "hamstercam/metrics.hpp"
#include "hamstercam/process_info.hpp"
#include "hamstercam/v4l2_capture_device.hpp"
#include "hamstercam/version.hpp"

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

void interruptible_sleep(std::chrono::milliseconds total) {
    constexpr auto step = 100ms;
    auto remaining = total;
    while (remaining > 0ms && !g_stop_requested.load(std::memory_order_relaxed)) {
        const auto chunk = std::min(remaining, step);
        std::this_thread::sleep_for(chunk);
        remaining -= chunk;
    }
}

const std::set<std::string>& known_flags() {
    static const std::set<std::string> flags = {"--device", "--metrics-addr", "--expected-fps",
                                                  "--stall-multiple", "--buffer-count",
                                                  "--reconnect-max-backoff", "--log-level"};
    return flags;
}

// Returns false and prints a message on any unrecognized flag or missing
// value: an unparseable command line is a startup misconfiguration that can
// never recover, so main() exits rather than treating it as a retryable
// state.
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
            std::cerr << "unknown flag: " << name << "\n";
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

bool parse_u32(const std::string& s, std::uint32_t& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        const unsigned long v = std::stoul(s, &pos);
        if (pos != s.size()) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_duration_ms(const std::string& s, std::chrono::milliseconds& out) {
    std::uint32_t v = 0;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "ms") == 0) {
        if (!parse_u32(s.substr(0, s.size() - 2), v)) return false;
        out = std::chrono::milliseconds(v);
        return true;
    }
    if (s.size() > 1 && s.back() == 's') {
        if (!parse_u32(s.substr(0, s.size() - 1), v)) return false;
        out = std::chrono::seconds(v);
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> flags;
    if (!parse_flags(argc, argv, flags)) return 1;

    const std::string device_path = resolve(flags, "--device", "HAMSTERCAM_DEVICE", "/dev/video40");
    const std::string metrics_addr = resolve(flags, "--metrics-addr", "HAMSTERCAM_METRICS_ADDR", "127.0.0.1:9843");
    const std::string log_level_str = resolve(flags, "--log-level", "HAMSTERCAM_LOG_LEVEL", "info");

    DaemonConfig config;
    if (!parse_u32(resolve(flags, "--expected-fps", "HAMSTERCAM_EXPECTED_FPS", "15"), config.expected_fps) ||
        !parse_u32(resolve(flags, "--stall-multiple", "HAMSTERCAM_STALL_MULTIPLE", "10"), config.stall_multiple) ||
        !parse_u32(resolve(flags, "--buffer-count", "HAMSTERCAM_BUFFER_COUNT", "4"), config.buffer_count) ||
        !parse_duration_ms(resolve(flags, "--reconnect-max-backoff", "HAMSTERCAM_RECONNECT_MAX_BACKOFF", "30s"),
                            config.reconnect_max_backoff)) {
        std::cerr << "invalid flag value\n";
        return 1;
    }

    LogLevel log_level;
    if (!parse_log_level(log_level_str, log_level)) {
        std::cerr << "invalid --log-level: " << log_level_str << "\n";
        return 1;
    }

    JsonLogger logger(std::cout, log_level);
    const BuildInfo build_info{kVersion, kGitSha};

    std::move(logger.log(LogLevel::Info, "startup"))
        .field("version", build_info.version)
        .field("git_sha", build_info.git_sha)
        .field("device", device_path)
        .field("metrics_addr", metrics_addr);

    V4L2CaptureDevice device(device_path);
    SteadyClock clock;
    CaptureStats stats;
    JsonEventSink event_sink(logger);
    CaptureStateMachine machine(device, clock, event_sink, stats, config);

    const auto start_time = std::chrono::steady_clock::now();

    HttpServer server(metrics_addr, [&](std::string_view method, std::string_view path) -> HttpResponse {
        if (method != "GET") return HttpResponse{404, "text/plain", "not found\n"};
        if (path == "/metrics") {
            const double uptime =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
            return HttpResponse{200, "text/plain; version=0.0.4; charset=utf-8",
                                 render_metrics(stats, build_info, uptime, resident_set_bytes())};
        }
        if (path == "/healthz") return HttpResponse{200, "text/plain", "ok\n"};
        return HttpResponse{404, "text/plain", "not found\n"};
    });

    if (!server.start()) {
        std::cerr << "failed to bind metrics address: " << metrics_addr << "\n";
        return 1;
    }

    install_signal_handlers();
    std::thread http_thread([&] { server.run(); });

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const auto outcome = machine.step(200ms);
        interruptible_sleep(outcome.recommended_wait);
    }

    device.stop_streaming();
    device.close();

    server.stop();
    http_thread.join();

    logger.log(LogLevel::Info, "shutdown");
    return 0;
}
