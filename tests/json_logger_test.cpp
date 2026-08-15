// SPDX-License-Identifier: MIT
#include "hamstercam/json_logger.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace hamstercam;

TEST(JsonLoggerTest, WritesTsLevelEventAndFields) {
    std::ostringstream out;
    JsonLogger logger(out, LogLevel::Debug);

    std::move(logger.log(LogLevel::Warn, "stall_detected")).field("count", 3u);

    const auto line = out.str();
    EXPECT_NE(line.find("\"level\":\"warn\""), std::string::npos);
    EXPECT_NE(line.find("\"event\":\"stall_detected\""), std::string::npos);
    EXPECT_NE(line.find("\"count\":3"), std::string::npos);
    EXPECT_NE(line.find("\"ts\":\""), std::string::npos);
    EXPECT_EQ(line.back(), '\n');
}

TEST(JsonLoggerTest, BelowMinLevelIsSuppressed) {
    std::ostringstream out;
    JsonLogger logger(out, LogLevel::Warn);

    logger.log(LogLevel::Info, "awaiting_producer");

    EXPECT_TRUE(out.str().empty());
}

TEST(JsonLoggerTest, EscapesQuotesAndBackslashesInStringFields) {
    std::ostringstream out;
    JsonLogger logger(out, LogLevel::Debug);

    std::move(logger.log(LogLevel::Info, "device_lost")).field("reason", "a\"b\\c");

    EXPECT_NE(out.str().find("\"reason\":\"a\\\"b\\\\c\""), std::string::npos);
}

TEST(JsonEventSinkTest, TranslatesFormatChangedWithOldAndNewFormat) {
    std::ostringstream out;
    JsonLogger logger(out, LogLevel::Debug);
    JsonEventSink sink(logger);

    StreamFormat previous{640, 480, 0x47504A4D};
    StreamFormat current{1280, 720, 0x47504A4D};
    sink.emit(Event{EventType::FormatChanged, current, previous});

    const auto line = out.str();
    EXPECT_NE(line.find("\"event\":\"format_changed\""), std::string::npos);
    EXPECT_NE(line.find("\"width\":1280"), std::string::npos);
    EXPECT_NE(line.find("\"previous_width\":640"), std::string::npos);
}
