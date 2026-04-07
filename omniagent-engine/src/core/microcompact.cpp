#include "microcompact.h"

#include <algorithm>

namespace omni::engine {

std::vector<Message> microcompact(
    const std::vector<Message>& messages,
    int preserve_tail,
    int max_result_chars)
{
    std::vector<Message> result = messages;

    const int total      = static_cast<int>(result.size());
    const int tail_start = std::max(0, total - preserve_tail);

    for (int i = 0; i < tail_start; ++i) {
        Message& msg = result[i];
        if (msg.role != Role::ToolResult) continue;

        for (ToolResult& tr : msg.tool_results) {
            if (static_cast<int>(tr.content.size()) > max_result_chars) {
                tr.content = tr.content.substr(0, max_result_chars) + "\n[truncated]";
            }
        }
    }

    return result;
}

}  // namespace omni::engine
