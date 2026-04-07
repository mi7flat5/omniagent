#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

struct CommandResult {
    bool        handled        = false;
    std::string response;
    bool        clear_messages = false;
};

/// A registered command handler.
using CommandHandler = std::function<CommandResult(const std::string& args)>;

struct CommandInfo {
    std::string    name;
    std::string    description;
    CommandHandler handler;
};

class CommandRegistry {
public:
    /// Register a custom command.
    void add(const std::string& name, const std::string& description, CommandHandler handler);

    /// Remove a command.
    void remove(const std::string& name);

    /// Try to handle a command. Returns {handled=false} if not found.
    CommandResult handle(const std::string& name, const std::string& args);

    /// List all registered commands with descriptions.
    std::vector<CommandInfo> list() const;

    /// Number of registered commands.
    size_t count() const;

private:
    mutable std::mutex                        mutex_;
    std::unordered_map<std::string, CommandInfo> commands_;
};

}  // namespace omni::engine
