#include "cli/repl.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

double parse_double_option(const std::string& raw_value, const char* name) {
    std::size_t parsed = 0;
    const double value = std::stod(raw_value, &parsed);
    if (parsed != raw_value.size()) {
        throw std::invalid_argument(std::string("invalid value for ") + name + ": " + raw_value);
    }
    return value;
}

int parse_int_option(const std::string& raw_value, const char* name) {
    std::size_t parsed = 0;
    const int value = std::stoi(raw_value, &parsed);
    if (parsed != raw_value.size() || value <= 0) {
        throw std::invalid_argument(std::string("invalid value for ") + name + ": " + raw_value);
    }
    return value;
}

int parse_non_negative_int_option(const std::string& raw_value, const char* name) {
    std::size_t parsed = 0;
    const int value = std::stoi(raw_value, &parsed);
    if (parsed != raw_value.size() || value < 0) {
        throw std::invalid_argument(std::string("invalid value for ") + name + ": " + raw_value);
    }
    return value;
}

omni::engine::cli::CliOptions parse_args(int argc, char** argv) {
    omni::engine::cli::CliOptions options;

    std::vector<std::string> args(argv + 1, argv + argc);
    std::size_t index = 0;
    if (index < args.size() && !args[index].empty() && args[index][0] != '-') {
        options.mode = args[index++];
    }

    while (index < args.size()) {
        const auto& arg = args[index++];
        auto require_value = [&](const char* name) -> const std::string& {
            if (index >= args.size()) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return args[index++];
        };

        if (arg == "--help") {
            std::cout << omni::engine::cli::usage_text(argv[0]);
            std::exit(0);
        }
        if (arg == "--project-id") {
            options.project_id = require_value("--project-id");
            continue;
        }
        if (arg == "--workspace-root") {
            options.workspace_root = require_value("--workspace-root");
            continue;
        }
        if (arg == "--cwd") {
            options.working_dir = require_value("--cwd");
            continue;
        }
        if (arg == "--storage-dir") {
            options.storage_dir = require_value("--storage-dir");
            continue;
        }
        if (arg == "--profile") {
            options.profile = require_value("--profile");
            continue;
        }
        if (arg == "--session-id") {
            options.session_id = require_value("--session-id");
            continue;
        }
        if (arg == "--run-id") {
            options.run_id = require_value("--run-id");
            continue;
        }
        if (arg == "--base-url") {
            options.base_url = require_value("--base-url");
            continue;
        }
        if (arg == "--model") {
            options.model = require_value("--model");
            continue;
        }
        if (arg == "--api-key") {
            options.api_key = require_value("--api-key");
            continue;
        }
        if (arg == "--max-context-tokens") {
            options.max_context_tokens = parse_int_option(require_value("--max-context-tokens"), "--max-context-tokens");
            continue;
        }
        if (arg == "--temperature") {
            options.temperature = parse_double_option(require_value("--temperature"), "--temperature");
            continue;
        }
        if (arg == "--top-p") {
            options.top_p = parse_double_option(require_value("--top-p"), "--top-p");
            continue;
        }
        if (arg == "--top-k") {
            options.top_k = parse_non_negative_int_option(require_value("--top-k"), "--top-k");
            continue;
        }
        if (arg == "--min-p") {
            options.min_p = parse_double_option(require_value("--min-p"), "--min-p");
            continue;
        }
        if (arg == "--presence-penalty") {
            options.presence_penalty = parse_double_option(require_value("--presence-penalty"), "--presence-penalty");
            continue;
        }
        if (arg == "--frequency-penalty") {
            options.frequency_penalty = parse_double_option(require_value("--frequency-penalty"), "--frequency-penalty");
            continue;
        }
        if (arg == "--max-tokens") {
            options.max_tokens = parse_int_option(require_value("--max-tokens"), "--max-tokens");
            continue;
        }
        if (arg == "--prompt") {
            options.prompt = require_value("--prompt");
            continue;
        }
        if (arg == "--approval-policy") {
            options.approval_policy = require_value("--approval-policy");
            continue;
        }
        if (arg == "--resume-input") {
            options.resume_input = require_value("--resume-input");
            continue;
        }

        throw std::invalid_argument("unknown argument: " + arg);
    }

    if (options.project_id.empty() || options.workspace_root.empty()
        || options.base_url.empty() || options.model.empty()) {
        throw std::invalid_argument("project-id, workspace-root, base-url, and model are required");
    }

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        return omni::engine::cli::run_cli(options);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cerr << omni::engine::cli::usage_text(argv[0]);
        return 1;
    }
}