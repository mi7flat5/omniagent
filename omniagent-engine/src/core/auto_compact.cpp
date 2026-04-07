#include "auto_compact.h"

#include "stream_assembler.h"

#include <omni/types.h>

#include <algorithm>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>

namespace omni::engine {

// ---------------------------------------------------------------------------
// estimate_tokens — rough approximation: total chars / 4
// ---------------------------------------------------------------------------

int estimate_tokens(const std::vector<Message>& messages) {
    int total_chars = 0;

    for (const auto& msg : messages) {
        // Content blocks: text, thinking, tool use (serialize input), image (data field)
        for (const auto& block : msg.content) {
            if (const auto* tc = std::get_if<TextContent>(&block.data)) {
                total_chars += static_cast<int>(tc->text.size());
            } else if (const auto* th = std::get_if<ThinkingContent>(&block.data)) {
                total_chars += static_cast<int>(th->text.size());
            } else if (const auto* tu = std::get_if<ToolUseContent>(&block.data)) {
                total_chars += static_cast<int>(tu->name.size());
                total_chars += static_cast<int>(tu->id.size());
                total_chars += static_cast<int>(tu->input.dump().size());
            } else if (const auto* img = std::get_if<ImageContent>(&block.data)) {
                // base64 data is ~4/3 the binary size; rough estimate is fine
                total_chars += static_cast<int>(img->data.size());
            }
        }

        // Tool results
        for (const auto& tr : msg.tool_results) {
            total_chars += static_cast<int>(tr.tool_use_id.size());
            total_chars += static_cast<int>(tr.content.size());
        }
    }

    return total_chars / 4;
}

// ---------------------------------------------------------------------------
// serialize_messages_for_summary — plain text representation for the prompt
// ---------------------------------------------------------------------------

static std::string serialize_messages_for_summary(const std::vector<Message>& messages) {
    std::ostringstream oss;

    for (const auto& msg : messages) {
        switch (msg.role) {
            case Role::User:      oss << "[User]\n";      break;
            case Role::Assistant: oss << "[Assistant]\n"; break;
            case Role::System:    oss << "[System]\n";    break;
            case Role::ToolResult: oss << "[ToolResult]\n"; break;
        }

        for (const auto& block : msg.content) {
            if (const auto* tc = std::get_if<TextContent>(&block.data)) {
                oss << tc->text << "\n";
            } else if (const auto* th = std::get_if<ThinkingContent>(&block.data)) {
                oss << "<thinking>" << th->text << "</thinking>\n";
            } else if (const auto* tu = std::get_if<ToolUseContent>(&block.data)) {
                oss << "<tool_use name=\"" << tu->name << "\" id=\"" << tu->id << "\">\n";
                oss << tu->input.dump() << "\n</tool_use>\n";
            } else if (const auto* img = std::get_if<ImageContent>(&block.data)) {
                oss << "<image media_type=\"" << img->media_type << "\" />\n";
            }
        }

        for (const auto& tr : msg.tool_results) {
            oss << "<tool_result id=\"" << tr.tool_use_id << "\"";
            if (tr.is_error) oss << " error=\"true\"";
            oss << ">\n" << tr.content << "\n</tool_result>\n";
        }

        oss << "\n";
    }

    return oss.str();
}

// ---------------------------------------------------------------------------
// auto_compact
// ---------------------------------------------------------------------------

CompactResult auto_compact(
    const std::vector<Message>& messages,
    LLMProvider&                compaction_provider,
    int                         max_context_tokens,
    const CompactConfig&        config)
{
    // Nothing to compact if no context limit is known
    if (max_context_tokens <= 0) {
        return {false, messages, ""};
    }

    const int estimated = estimate_tokens(messages);
    const int threshold = static_cast<int>(
        static_cast<float>(max_context_tokens) * config.soft_limit_pct);

    if (estimated < threshold) {
        return {false, messages, ""};
    }

    // Clamp preserve_tail to messages size
    const int n         = static_cast<int>(messages.size());
    const int tail_size = std::min(config.preserve_tail, n);
    const int head_size = n - tail_size;

    // If there's nothing to summarize, return as-is
    if (head_size <= 0) {
        return {false, messages, ""};
    }

    // Split
    std::vector<Message> to_summarize(messages.begin(), messages.begin() + head_size);
    std::vector<Message> to_preserve(messages.begin() + head_size, messages.end());

    // Build the summarization prompt
    const std::string conversation_text = serialize_messages_for_summary(to_summarize);
    const std::string prompt_text =
        "Summarize this conversation concisely. Preserve key decisions, code changes, "
        "file paths mentioned, and tool results. Be brief but complete.\n\n"
        "Conversation:\n" + conversation_text;

    // Build a minimal completion request — no tools, no system prompt
    CompletionRequest summary_req;
    summary_req.max_tokens = config.max_summary_tokens;

    Message user_msg;
    user_msg.role    = Role::User;
    user_msg.content = {ContentBlock{TextContent{prompt_text}}};
    summary_req.messages = {user_msg};

    // Collect text from the streaming response
    std::string summary;
    StreamAssembler assembler;

    auto stream_cb = [&](const StreamEventData& ev) {
        assembler.process(ev);
    };

    std::atomic<bool> no_cancel{false};
    try {
        compaction_provider.complete(summary_req, stream_cb, no_cancel);
    } catch (const std::exception&) {
        // If compaction fails, return the original messages unchanged
        return {false, messages, ""};
    }

    // Extract text from assembled blocks
    for (const auto& block : assembler.take_completed_blocks()) {
        if (const auto* tc = std::get_if<TextContent>(&block.data)) {
            summary += tc->text;
        }
    }

    if (summary.empty()) {
        return {false, messages, ""};
    }

    // Build the compacted message list:
    //   [System summary message] + to_preserve
    Message summary_msg;
    summary_msg.role    = Role::System;
    summary_msg.content = {ContentBlock{TextContent{
        "[Context Summary]\n" + summary + "\n[End Summary]"
    }}};

    std::vector<Message> compacted;
    compacted.reserve(1 + to_preserve.size());
    compacted.push_back(std::move(summary_msg));
    for (auto& m : to_preserve) {
        compacted.push_back(std::move(m));
    }

    return {true, std::move(compacted), std::move(summary)};
}

// ---------------------------------------------------------------------------
// reactive_compact — always compacts, minimal tail
// ---------------------------------------------------------------------------

CompactResult reactive_compact(
    const std::vector<Message>& messages,
    LLMProvider&                compaction_provider,
    int                         max_context_tokens)
{
    CompactConfig aggressive;
    aggressive.soft_limit_pct = 0.0f;   // always compact
    aggressive.preserve_tail  = 2;      // keep only last 2 messages
    aggressive.max_summary_tokens = 1024;

    return auto_compact(messages, compaction_provider, max_context_tokens, aggressive);
}

}  // namespace omni::engine
