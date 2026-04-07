#pragma once

#include <omni/tool.h>

#include <memory>
#include <vector>

namespace omni::engine {

class ReadFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr std::size_t kMaxBytes = 16 * 1024;
};

class WriteFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;
};

class EditFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;
};

class DeleteFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;
};

class ListDirTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr std::size_t kMaxEntries = 1000;
};

class GlobTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr std::size_t kMaxResults = 500;
};

class GrepTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr std::size_t kMaxOutputBytes = 48 * 1024;
};

class BashTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return true; }
    bool is_shell() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr int kDefaultTimeoutMs = 120000;
    static constexpr std::size_t kMaxOutputBytes = 64 * 1024;
};

std::vector<std::unique_ptr<Tool>> make_default_workspace_tools();

}  // namespace omni::engine