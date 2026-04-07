#include "slash_commands.h"
#include "commands/command_registry.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Built-in command descriptors (name + description), used by /help.
// ---------------------------------------------------------------------------
static constexpr std::pair<std::string_view, std::string_view> k_builtins[] = {
    {"clear",    "Clear conversation history"},
    {"compact",  "Compact conversation history"},
    {"messages", "Show message count and token usage"},
    {"model",    "Show the active model name"},
    {"cost",     "Show cumulative token cost summary"},
    {"help",     "List all available commands"},
};

// ---------------------------------------------------------------------------
// handle_slash_command
// ---------------------------------------------------------------------------
SlashCommandResult handle_slash_command(
    const std::string&          text,
    const std::vector<Message>& messages,
    const Usage&                usage,
    const std::string&          model_name,
    CommandRegistry*            custom_commands)
{
    // Not a slash command if text doesn't start with '/'.
    if (text.empty() || text[0] != '/') {
        return SlashCommandResult{};
    }

    // Extract command name and args.
    // Format: "/command args here"
    const std::string_view sv{text};
    const auto space_pos = sv.find(' ', 1);
    const std::string cmd = std::string(
        space_pos == std::string_view::npos
            ? sv.substr(1)
            : sv.substr(1, space_pos - 1));
    const std::string args = (space_pos == std::string_view::npos)
        ? ""
        : std::string(sv.substr(space_pos + 1));

    // ------------------------------------------------------------------
    // Built-in: /clear
    // ------------------------------------------------------------------
    if (cmd == "clear") {
        return {
            .handled        = true,
            .clear_messages = true,
            .response_text  = "Conversation cleared.",
        };
    }

    // ------------------------------------------------------------------
    // Built-in: /compact
    // ------------------------------------------------------------------
    if (cmd == "compact") {
        return {
            .handled        = true,
            .clear_messages = false,
            .response_text  = std::format(
                "Compacting conversation (preserve_tail=2, max_result_chars=100). "
                "{} messages in history.",
                messages.size()),
        };
    }

    // ------------------------------------------------------------------
    // Built-in: /messages
    // ------------------------------------------------------------------
    if (cmd == "messages") {
        const int64_t total_tokens =
            usage.input_tokens + usage.output_tokens + usage.cache_read_tokens;
        return {
            .handled        = true,
            .clear_messages = false,
            .response_text  = std::format(
                "Messages: {} | Tokens used: {} (in={}, out={}, cached={})",
                messages.size(),
                total_tokens,
                usage.input_tokens,
                usage.output_tokens,
                usage.cache_read_tokens),
        };
    }

    // ------------------------------------------------------------------
    // Built-in: /model
    // ------------------------------------------------------------------
    if (cmd == "model") {
        const std::string name = model_name.empty() ? "(not set)" : model_name;
        return {
            .handled        = true,
            .clear_messages = false,
            .response_text  = std::format("Active model: {}", name),
        };
    }

    // ------------------------------------------------------------------
    // Built-in: /cost
    // ------------------------------------------------------------------
    if (cmd == "cost") {
        const int64_t total = usage.input_tokens + usage.output_tokens + usage.cache_read_tokens;
        return {
            .handled        = true,
            .clear_messages = false,
            .response_text  = std::format(
                "Token usage — input: {}, output: {}, cache_read: {}, total: {}",
                usage.input_tokens,
                usage.output_tokens,
                usage.cache_read_tokens,
                total),
        };
    }

    // ------------------------------------------------------------------
    // Built-in: /help
    // ------------------------------------------------------------------
    if (cmd == "help") {
        std::string text_out = "Available commands:\n";

        // Built-ins.
        for (const auto& [name, desc] : k_builtins) {
            text_out += std::format("  /{:<12} {}\n", name, desc);
        }

        // Custom commands from registry.
        if (custom_commands != nullptr) {
            auto custom = custom_commands->list();
            // Sort by name for deterministic output.
            std::sort(custom.begin(), custom.end(),
                [](const CommandInfo& a, const CommandInfo& b) { return a.name < b.name; });
            for (const auto& info : custom) {
                text_out += std::format("  /{:<12} {}\n", info.name, info.description);
            }
        }

        return {
            .handled        = true,
            .clear_messages = false,
            .response_text  = std::move(text_out),
        };
    }

    // ------------------------------------------------------------------
    // Delegate to custom command registry (if provided).
    // ------------------------------------------------------------------
    if (custom_commands != nullptr) {
        CommandResult cr = custom_commands->handle(cmd, args);
        if (cr.handled) {
            return {
                .handled        = true,
                .clear_messages = cr.clear_messages,
                .response_text  = std::move(cr.response),
            };
        }
    }

    // Unrecognized command — treat as normal user message.
    return SlashCommandResult{};
}

}  // namespace omni::engine
