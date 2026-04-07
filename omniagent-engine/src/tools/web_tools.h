#pragma once

#include <omni/tool.h>

#include <string>

namespace omni::engine {

class WebFetchTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr std::size_t kMaxBytes = 16 * 1024;
};

class WebSearchTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr int kDefaultCount = 5;
    static constexpr int kMaxCount = 10;
};

}  // namespace omni::engine