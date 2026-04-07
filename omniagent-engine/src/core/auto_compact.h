#pragma once

#include <omni/provider.h>
#include <omni/types.h>
#include <string>
#include <vector>

namespace omni::engine {

struct CompactConfig {
    float soft_limit_pct     = 0.75f;  // trigger auto-compact at 75% of context window
    int   preserve_tail      = 6;      // keep last N messages untouched
    int   max_summary_tokens = 1024;   // max tokens for the summary
};

struct CompactResult {
    bool                 compacted = false;  // true if compaction happened
    std::vector<Message> messages;           // the compacted message list
    std::string          summary;            // the generated summary (for logging)
};

/// Estimate token count for a message list (rough: ~4 chars per token).
int estimate_tokens(const std::vector<Message>& messages);

/// Auto-compact: if estimated tokens exceed soft_limit_pct of max_context_tokens,
/// call the compaction provider to summarize old messages.
/// Returns the compacted message list if compaction happened.
CompactResult auto_compact(
    const std::vector<Message>& messages,
    LLMProvider&                compaction_provider,
    int                         max_context_tokens,
    const CompactConfig&        config = {});

/// Reactive compact: more aggressive version triggered by context overflow.
/// Uses smaller preserve_tail (2) and stricter limits.
CompactResult reactive_compact(
    const std::vector<Message>& messages,
    LLMProvider&                compaction_provider,
    int                         max_context_tokens);

}  // namespace omni::engine
