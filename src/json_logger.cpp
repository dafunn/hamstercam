// SPDX-License-Identifier: MIT
#include "hamstercam/json_logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace hamstercam {

namespace {

std::string_view level_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "info";
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string rfc3339_now() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    gmtime_r(&t, &tm);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

}  // namespace

bool parse_log_level(std::string_view text, LogLevel& out) {
    if (text == "debug") {
        out = LogLevel::Debug;
    } else if (text == "info") {
        out = LogLevel::Info;
    } else if (text == "warn") {
        out = LogLevel::Warn;
    } else if (text == "error") {
        out = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

JsonLogger::JsonLogger(std::ostream& out, LogLevel min_level) : out_(out), min_level_(min_level) {}

JsonLogger::Line::Line(std::ostream* out, std::string prefix) : out_(out), buf_(std::move(prefix)) {}

JsonLogger::Line::~Line() {
    if (out_ == nullptr) return;
    buf_ += "}\n";
    *out_ << buf_;
    out_->flush();
}

JsonLogger::Line&& JsonLogger::Line::field(std::string_view key, std::string_view value) && {
    if (out_ != nullptr) {
        buf_ += ",\"";
        buf_ += key;
        buf_ += "\":\"";
        buf_ += escape(value);
        buf_ += "\"";
    }
    return std::move(*this);
}

JsonLogger::Line&& JsonLogger::Line::field(std::string_view key, std::uint32_t value) && {
    if (out_ != nullptr) {
        buf_ += ",\"";
        buf_ += key;
        buf_ += "\":";
        buf_ += std::to_string(value);
    }
    return std::move(*this);
}

JsonLogger::Line JsonLogger::log(LogLevel level, std::string_view event) {
    if (level < min_level_) return Line(nullptr, "");

    std::string prefix = "{\"ts\":\"";
    prefix += rfc3339_now();
    prefix += "\",\"level\":\"";
    prefix += level_string(level);
    prefix += "\",\"event\":\"";
    prefix += event;
    prefix += "\"";
    return Line(&out_, std::move(prefix));
}

void JsonEventSink::emit(const Event& event) {
    switch (event.type) {
        case EventType::DeviceOpened:
            logger_.log(LogLevel::Info, "device_opened");
            return;
        case EventType::AwaitingProducer:
            logger_.log(LogLevel::Info, "awaiting_producer");
            return;
        case EventType::StreamingStarted:
            std::move(logger_.log(LogLevel::Info, "streaming_started"))
                .field("width", event.format.width)
                .field("height", event.format.height)
                .field("pixel_format", event.format.pixel_format_string());
            return;
        case EventType::DeviceLost:
            std::move(logger_.log(LogLevel::Warn, "device_lost")).field("reason", to_string(event.reason));
            return;
        case EventType::Reconnecting:
            logger_.log(LogLevel::Info, "reconnecting");
            return;
        case EventType::StallDetected:
            logger_.log(LogLevel::Warn, "stall_detected");
            return;
        case EventType::StallCleared:
            logger_.log(LogLevel::Info, "stall_cleared");
            return;
        case EventType::FormatChanged:
            std::move(logger_.log(LogLevel::Info, "format_changed"))
                .field("width", event.format.width)
                .field("height", event.format.height)
                .field("pixel_format", event.format.pixel_format_string())
                .field("previous_width", event.previous_format.width)
                .field("previous_height", event.previous_format.height)
                .field("previous_pixel_format", event.previous_format.pixel_format_string());
            return;
    }
}

}  // namespace hamstercam
