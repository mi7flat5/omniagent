#pragma once

#include <omni/tool.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

class ToolRegistry {
public:
    void register_tool(std::unique_ptr<Tool> tool);
    /// Remove a previously registered tool by name.
    /// Returns true if a tool with that name existed and was removed.
    bool unregister_tool(const std::string& name);
    Tool* get(const std::string& name) const;
    std::vector<std::string>    names() const;
    std::vector<nlohmann::json> tool_definitions() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace omni::engine
