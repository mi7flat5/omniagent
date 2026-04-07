#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace omni::engine::cli {

struct CliOptions {
    std::string mode = "repl";
    std::string project_id;
    std::filesystem::path workspace_root;
    std::optional<std::filesystem::path> working_dir;
    std::optional<std::filesystem::path> storage_dir;
    std::string profile = "coordinator";
    std::optional<std::string> session_id;
    std::optional<std::string> run_id;
    std::string base_url;
    std::string model;
    std::string api_key;
    std::optional<std::string> prompt;
    std::optional<std::string> approval_policy;
    std::string resume_input = "approve";
};

std::string usage_text(const char* program_name);
int run_cli(const CliOptions& options);

}  // namespace omni::engine::cli