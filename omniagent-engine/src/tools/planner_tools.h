#pragma once

#include <omni/tool.h>

#include <string>

namespace omni::engine {

class PlannerValidateSpecTool : public Tool {
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

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerValidatePlanTool : public Tool {
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

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerValidateReviewTool : public Tool {
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

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerValidateBugfixTool : public Tool {
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

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerRepairPlanTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerBuildPlanTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr int kDefaultTimeoutMs = 600000;
};

class PlannerBuildFromIdeaTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json& args,
                        const ToolContext& context) override;
    ToolCallResult call(const nlohmann::json& args) override;

    static constexpr int kDefaultTimeoutMs = 900000;
};

}  // namespace omni::engine