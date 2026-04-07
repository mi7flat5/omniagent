#pragma once

#include <omni/types.h>
#include <string>
#include <vector>

namespace omni::engine {

// Forward declaration — callers that pass a registry must include command_registry.h
class CommandRegistry;

struct SlashCommandResult {
    bool        handled        = false;   // true if command was recognized
    bool        clear_messages = false;   // true for /clear
    std::string response_text;            // text to emit to observer (if any)
};

/// Parse and handle a slash command. Returns {handled=false} if not a slash command.
///
/// @param text            The raw user input (must start with '/').
/// @param messages        Current conversation history (read-only).
/// @param usage           Cumulative token usage for /cost and /messages.
/// @param model_name      Active model identifier, shown by /model.
/// @param custom_commands Optional host-registered command registry checked after built-ins.
SlashCommandResult handle_slash_command(
    const std::string&          text,
    const std::vector<Message>& messages,
    const Usage&                usage,
    const std::string&          model_name     = "",
    CommandRegistry*            custom_commands = nullptr);

}  // namespace omni::engine
