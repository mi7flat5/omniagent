#pragma once

#include <omni/event.h>

#include <string>

namespace omni::engine {

class RunObserver {
public:
    virtual ~RunObserver() = default;

    virtual void on_event(const Event& event,
                          const std::string& project_id,
                          const std::string& session_id,
                          const std::string& run_id) = 0;
};

}  // namespace omni::engine