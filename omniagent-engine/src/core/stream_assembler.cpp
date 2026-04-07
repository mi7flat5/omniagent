#include "stream_assembler.h"

namespace omni::engine {

void StreamAssembler::process(const StreamEventData& event) {
    switch (event.type) {
        case StreamEventType::MessageStart:
            // no-op
            break;

        case StreamEventType::ContentBlockStart: {
            InProgressBlock blk;
            blk.index     = event.index;
            blk.type      = event.delta_type;  // type carried in delta_type at start
            blk.tool_id   = event.tool_id;
            blk.tool_name = event.tool_name;
            // If the start event already carries a complete input, capture it
            if (!event.tool_input.is_null()) {
                blk.tool_input = event.tool_input;
            }
            in_progress_[event.index] = std::move(blk);
            break;
        }

        case StreamEventType::ContentBlockDelta: {
            auto it = in_progress_.find(event.index);
            if (it == in_progress_.end()) break;

            InProgressBlock& blk = it->second;
            if (event.delta_type == "text_delta" || event.delta_type == "thinking_delta") {
                blk.text += event.delta_text;
            } else if (event.delta_type == "input_json_delta") {
                // Partial JSON string — accumulate as raw text then merge at stop
                if (blk.tool_input.is_null()) {
                    blk.tool_input = event.tool_input_delta;
                } else {
                    blk.tool_input.merge_patch(event.tool_input_delta);
                }
            }
            break;
        }

        case StreamEventType::ContentBlockStop: {
            auto it = in_progress_.find(event.index);
            if (it == in_progress_.end()) break;

            InProgressBlock& blk = it->second;

            // Prefer a complete input from the stop event if provided
            nlohmann::json final_input =
                (!event.tool_input.is_null()) ? event.tool_input : blk.tool_input;

            if (blk.type == "text") {
                completed_.push_back(ContentBlock{TextContent{blk.text}});
            } else if (blk.type == "thinking") {
                completed_.push_back(ContentBlock{ThinkingContent{blk.text}});
            } else if (blk.type == "tool_use") {
                ToolUseContent tuc;
                tuc.id    = blk.tool_id;
                tuc.name  = blk.tool_name;
                tuc.input = final_input.is_null() ? nlohmann::json::object() : final_input;
                completed_.push_back(ContentBlock{std::move(tuc)});
            }

            in_progress_.erase(it);
            break;
        }

        case StreamEventType::MessageDelta:
            stop_reason_ = event.stop_reason;
            final_usage_ = event.usage;
            break;

        case StreamEventType::MessageStop:
            // no-op
            break;
    }
}

std::vector<ContentBlock> StreamAssembler::take_completed_blocks() {
    std::vector<ContentBlock> out;
    out.swap(completed_);
    return out;
}

void StreamAssembler::reset() {
    in_progress_.clear();
    completed_.clear();
    stop_reason_.clear();
    final_usage_ = {};
}

}  // namespace omni::engine
