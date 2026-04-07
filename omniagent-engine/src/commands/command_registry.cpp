#include "command_registry.h"

#include <stdexcept>

namespace omni::engine {

void CommandRegistry::add(const std::string& name,
                          const std::string& description,
                          CommandHandler      handler)
{
    if (name.empty()) {
        throw std::invalid_argument("command name must not be empty");
    }
    std::lock_guard lock{mutex_};
    commands_[name] = CommandInfo{name, description, std::move(handler)};
}

void CommandRegistry::remove(const std::string& name)
{
    std::lock_guard lock{mutex_};
    commands_.erase(name);
}

CommandResult CommandRegistry::handle(const std::string& name, const std::string& args)
{
    std::lock_guard lock{mutex_};
    const auto it = commands_.find(name);
    if (it == commands_.end()) {
        return CommandResult{};
    }
    return it->second.handler(args);
}

std::vector<CommandInfo> CommandRegistry::list() const
{
    std::lock_guard lock{mutex_};
    std::vector<CommandInfo> result;
    result.reserve(commands_.size());
    for (const auto& [_, info] : commands_) {
        result.push_back(info);
    }
    return result;
}

size_t CommandRegistry::count() const
{
    std::lock_guard lock{mutex_};
    return commands_.size();
}

}  // namespace omni::engine
