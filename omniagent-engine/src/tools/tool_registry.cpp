#include "tool_registry.h"

namespace omni::engine {

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    std::lock_guard lock(mutex_);
    const std::string key = tool->name();
    tools_[key] = std::move(tool);
}

bool ToolRegistry::unregister_tool(const std::string& name) {
    std::lock_guard lock(mutex_);
    return tools_.erase(name) > 0;
}

Tool* ToolRegistry::get(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = tools_.find(name);
    return (it != tools_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> ToolRegistry::names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    result.reserve(tools_.size());
    for (const auto& [k, _] : tools_) {
        result.push_back(k);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<nlohmann::json> ToolRegistry::tool_definitions() const {
    std::lock_guard lock(mutex_);

    // Collect and sort names under a single lock to avoid TOCTOU race
    std::vector<std::string> sorted_names;
    sorted_names.reserve(tools_.size());
    for (const auto& [k, _] : tools_) {
        sorted_names.push_back(k);
    }
    std::sort(sorted_names.begin(), sorted_names.end());

    std::vector<nlohmann::json> defs;
    defs.reserve(sorted_names.size());
    for (const auto& n : sorted_names) {
        const Tool* t = tools_.at(n).get();
        defs.push_back({
            {"type", "function"},
            {"function", {
                {"name",        t->name()},
                {"description", t->description()},
                {"parameters",  t->input_schema()}
            }}
        });
    }
    return defs;
}

size_t ToolRegistry::size() const {
    std::lock_guard lock(mutex_);
    return tools_.size();
}

}  // namespace omni::engine
