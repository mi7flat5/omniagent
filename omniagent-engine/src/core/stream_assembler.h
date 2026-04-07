#pragma once

#include <omni/provider.h>
#include <omni/types.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

class StreamAssembler {
public:
    void process(const StreamEventData& event);
    std::vector<ContentBlock> take_completed_blocks();
    const std::string& stop_reason() const { return stop_reason_; }
    const Usage& final_usage() const { return final_usage_; }
    void reset();

private:
    struct InProgressBlock {
        int            index = 0;
        std::string    type;       // "text", "thinking", "tool_use"
        std::string    text;       // accumulated text
        std::string    tool_id;
        std::string    tool_name;
        std::string    tool_input_json_text;
        nlohmann::json tool_input;
    };

    std::unordered_map<int, InProgressBlock> in_progress_;
    std::vector<ContentBlock>                completed_;
    std::string                              stop_reason_;
    Usage                                    final_usage_;
};

}  // namespace omni::engine
