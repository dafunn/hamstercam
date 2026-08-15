// SPDX-License-Identifier: MIT
#pragma once

#include <vector>

#include "hamstercam/event.hpp"

namespace hamstercam::testing {

class RecordingEventSink final : public EventSink {
public:
    void emit(const Event& event) override { events.push_back(event); }

    std::vector<Event> events;
};

}  // namespace hamstercam::testing
