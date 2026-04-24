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
    std::optional<int> max_context_tokens;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> top_k;
    std::optional<double> min_p;
    std::optional<double> presence_penalty;
    std::optional<double> frequency_penalty;
    std::optional<int> max_tokens;
    std::optional<std::string> prompt;
    std::optional<std::string> approval_policy;
    std::string resume_input = "approve";
};

std::string repl_help_text();
std::string usage_text(const char* program_name);
int run_cli(const CliOptions& options);

}  // namespace omni::engine::cli