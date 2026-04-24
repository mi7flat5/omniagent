#pragma once

#include <omni/types.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

enum class StreamEventType {
    MessageStart,
    ContentBlockStart,
    ContentBlockDelta,
    ContentBlockStop,
    MessageDelta,
    MessageStop,
};

struct StreamEventData {
    StreamEventType type;
    int             index           = 0;
    std::string     delta_type;
    std::string     delta_text;
    std::string     tool_id;
    std::string     tool_name;
    nlohmann::json  tool_input_delta;
    nlohmann::json  tool_input;
    std::string     stop_reason;
    Usage           usage;
};

struct CompletionRequest {
    std::vector<Message>        messages;
    std::string                 system_prompt;
    std::vector<nlohmann::json> tools;
    std::optional<std::string>  tool_choice;
    std::optional<double>       temperature;
    std::optional<double>       top_p;
    std::optional<int>          top_k;
    std::optional<double>       min_p;
    std::optional<double>       presence_penalty;
    std::optional<double>       frequency_penalty;
    std::optional<int>          max_tokens;
    std::vector<std::string>    stop_sequences;
};

using StreamCallback = std::function<void(const StreamEventData& event)>;

class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    virtual Usage complete(const CompletionRequest& request,
                           StreamCallback stream_cb,
                           std::atomic<bool>& stop_flag) = 0;

    virtual ModelCapabilities capabilities() const = 0;
    virtual std::string name() const = 0;
};

}  // namespace omni::engine
