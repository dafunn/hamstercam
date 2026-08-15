// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "hamstercam/event.hpp"

namespace hamstercam {

enum class LogLevel { Debug, Info, Warn, Error };

bool parse_log_level(std::string_view text, LogLevel& out);

class JsonLogger {
public:
    JsonLogger(std::ostream& out, LogLevel min_level);

    // Builds one JSON line: ts, level, event, then chained fields, flushed
    // on destruction. Silently a no-op if level is below the configured
    // minimum.
    class Line {
    public:
        Line(Line&&) = default;
        ~Line();

        Line&& field(std::string_view key, std::string_view value) &&;
        Line&& field(std::string_view key, std::uint32_t value) &&;

    private:
        friend class JsonLogger;
        Line(std::ostream* out, std::string prefix);

        std::ostream* out_;
        std::string buf_;
    };

    Line log(LogLevel level, std::string_view event);

private:
    std::ostream& out_;
    LogLevel min_level_;
};

// Translates capture state-machine Events into JsonLogger calls.
class JsonEventSink final : public EventSink {
public:
    explicit JsonEventSink(JsonLogger& logger) : logger_(logger) {}
    void emit(const Event& event) override;

private:
    JsonLogger& logger_;
};

}  // namespace hamstercam
