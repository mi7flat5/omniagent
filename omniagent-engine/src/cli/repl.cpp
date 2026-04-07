#include "cli/repl.h"

#include <omni/approval.h>
#include <omni/event.h>
#include <omni/host.h>
#include <omni/observer.h>
#include <omni/project_session.h>
#include <omni/run.h>

#include "providers/http_provider.h"
#include "tools/workspace_tools.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace omni::engine::cli {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

std::filesystem::path default_storage_dir(const CliOptions& options) {
    return options.workspace_root / ".omniagent" / "engine-cli";
}

std::string run_status_name(RunStatus status) {
    switch (status) {
        case RunStatus::Running:
            return "running";
        case RunStatus::Paused:
            return "paused";
        case RunStatus::Completed:
            return "completed";
        case RunStatus::Stopped:
            return "stopped";
        case RunStatus::Cancelled:
            return "cancelled";
        case RunStatus::Failed:
            return "failed";
    }
    return "unknown";
}

json usage_json(const Usage& usage) {
    return {
        {"input_tokens", usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"cache_read_tokens", usage.cache_read_tokens},
    };
}

json tool_summary_json(const ToolSummary& summary) {
    return {
        {"name", summary.name},
        {"read_only", summary.read_only},
        {"destructive", summary.destructive},
        {"shell", summary.shell},
        {"network", summary.network},
        {"mcp", summary.mcp},
        {"sub_agent", summary.sub_agent},
    };
}

json session_summary_json(const SessionSummary& summary) {
    return {
        {"session_id", summary.session_id},
        {"project_id", summary.project_id},
        {"message_count", summary.message_count},
        {"active_profile", summary.active_profile},
        {"working_dir", summary.working_dir.string()},
        {"updated_at", summary.updated_at},
    };
}

json session_snapshot_json(const SessionSnapshot& snapshot,
                          const std::vector<ToolSummary>& tools) {
    json tool_items = json::array();
    for (const auto& tool : tools) {
        tool_items.push_back(tool_summary_json(tool));
    }

    json messages = json::array();
    for (const auto& message : snapshot.messages) {
        messages.push_back(message.to_json());
    }

    return {
        {"session_id", snapshot.session_id},
        {"project_id", snapshot.project_id},
        {"active_profile", snapshot.active_profile},
        {"working_dir", snapshot.working_dir.string()},
        {"messages", std::move(messages)},
        {"usage", usage_json(snapshot.usage)},
        {"active_run_id", snapshot.active_run_id.has_value() ? json(*snapshot.active_run_id) : json(nullptr)},
        {"tools", std::move(tool_items)},
    };
}

json run_summary_json(const RunSummary& summary) {
    return {
        {"run_id", summary.run_id},
        {"session_id", summary.session_id},
        {"project_id", summary.project_id},
        {"profile", summary.profile},
        {"status", run_status_name(summary.status)},
    };
}

json run_result_json(const RunResult& result) {
    json payload = {
        {"run_id", result.run_id},
        {"session_id", result.session_id},
        {"project_id", result.project_id},
        {"profile", result.profile},
        {"status", run_status_name(result.status)},
        {"input", result.input},
        {"output", result.output},
        {"usage", usage_json(result.usage)},
        {"error", result.error.has_value() ? json(*result.error) : json(nullptr)},
        {"pause_reason", result.pause_reason.has_value() ? json(*result.pause_reason) : json(nullptr)},
        {"pending_approval", nullptr},
    };

    if (result.pending_approval.has_value()) {
        payload["pending_approval"] = {
            {"tool_name", result.pending_approval->tool_name},
            {"args", result.pending_approval->args},
            {"description", result.pending_approval->description},
        };
    }

    return payload;
}

void print_json(const json& value) {
    std::cout << value.dump(2) << '\n';
}

enum class RunWaitOutcome {
    Missing,
    Paused,
    Finished,
};

RunWaitOutcome wait_for_run_block_or_finish(ProjectEngineHost& host,
                                            const std::string& run_id,
                                            std::function<void()> on_running = {},
                                            std::chrono::milliseconds timeout = 2min) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto run = host.get_run(run_id);
        if (!run.has_value()) {
            return RunWaitOutcome::Missing;
        }

        switch (run->status) {
            case RunStatus::Running:
                if (on_running) {
                    on_running();
                }
                std::this_thread::sleep_for(25ms);
                continue;
            case RunStatus::Paused:
                return RunWaitOutcome::Paused;
            case RunStatus::Completed:
            case RunStatus::Stopped:
            case RunStatus::Cancelled:
            case RunStatus::Failed:
                return RunWaitOutcome::Finished;
        }
    }
    return RunWaitOutcome::Missing;
}

class TerminalObserver : public RunObserver {
public:
    void on_event(const Event& event,
                  const std::string&,
                  const std::string&,
                  const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (const auto* started = std::get_if<RunStartedEvent>(&event)) {
            active_run_id_ = started->run_id;
            wait_started_at_ = std::chrono::steady_clock::now();
            last_status_at_ = wait_started_at_;
            saw_visible_activity_ = false;
            printed_thinking_ = false;
            std::cout << "\n[run started: " << started->run_id << "]\n" << std::flush;
        } else if (const auto* text = std::get_if<TextDeltaEvent>(&event)) {
            saw_visible_activity_ = true;
            std::cout << text->text << std::flush;
        } else if (std::holds_alternative<ThinkingDeltaEvent>(event)) {
            if (!printed_thinking_) {
                printed_thinking_ = true;
                last_status_at_ = std::chrono::steady_clock::now();
                std::cout << "\n[thinking]\n" << std::flush;
            }
        } else if (const auto* error = std::get_if<ErrorEvent>(&event)) {
            saw_visible_activity_ = true;
            std::cerr << "\nerror: " << error->message << '\n';
        } else if (const auto* tool = std::get_if<ToolUseStartEvent>(&event)) {
            saw_visible_activity_ = true;
            std::cout << "\n[tool] " << tool->name << '\n';
        } else if (const auto* paused = std::get_if<RunPausedEvent>(&event)) {
            saw_visible_activity_ = true;
            std::cout << "\n[run paused: " << paused->run_id << "]\n" << std::flush;
        }
    }

    void note_wait_tick(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (active_run_id_.empty()) {
            active_run_id_ = run_id;
            wait_started_at_ = now;
            last_status_at_ = now;
            saw_visible_activity_ = false;
            printed_thinking_ = false;
            std::cout << "\n[run started: " << run_id << "]\n" << std::flush;
            return;
        }
        if (saw_visible_activity_ || now - last_status_at_ < 2s) {
            return;
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - wait_started_at_).count();
        std::cout << "\n[still working " << seconds << "s]\n" << std::flush;
        last_status_at_ = now;
    }

    void finish_waiting() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_run_id_.clear();
        saw_visible_activity_ = false;
        printed_thinking_ = false;
        wait_started_at_ = std::chrono::steady_clock::time_point{};
        last_status_at_ = std::chrono::steady_clock::time_point{};
    }

private:
    std::mutex mutex_;
    std::string active_run_id_;
    std::chrono::steady_clock::time_point wait_started_at_{};
    std::chrono::steady_clock::time_point last_status_at_{};
    bool saw_visible_activity_ = false;
    bool printed_thinking_ = false;
};

class TerminalApprovals : public ApprovalDelegate {
public:
    ApprovalDecision on_approval_requested(const std::string& tool_name,
                                           const nlohmann::json& args,
                                           const std::string& description) override {
        std::cout << "\napproval required for tool '" << tool_name << "'\n";
        std::cout << description << '\n';
        if (!args.is_null()) {
            std::cout << args.dump(2) << '\n';
        }
        std::cout << "approve? [y/N] " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            return ApprovalDecision::Deny;
        }
        return (!line.empty() && (line[0] == 'y' || line[0] == 'Y'))
            ? ApprovalDecision::Approve
            : ApprovalDecision::Deny;
    }
};

class PauseForResumeApprovals : public ApprovalDelegate {
public:
    ApprovalDecision on_approval_requested(const std::string& tool_name,
                                           const nlohmann::json& args,
                                           const std::string& description) override {
        std::cout << "\napproval required for tool '" << tool_name << "'\n";
        std::cout << description << '\n';
        if (!args.is_null()) {
            std::cout << args.dump(2) << '\n';
        }
        std::cout << "run paused; use /resume [approve|deny|approve_always], /stop, or /cancel\n";
        return ApprovalDecision::Pause;
    }
};

ProjectRuntimeConfig build_config(const CliOptions& options) {
    ProjectRuntimeConfig config;
    config.workspace.project_id = options.project_id;
    config.workspace.workspace_root = options.workspace_root;
    config.workspace.working_dir = options.working_dir.value_or(options.workspace_root);
    config.engine.session_storage_dir = options.storage_dir.value_or(default_storage_dir(options));
    config.project_tools = make_default_workspace_tools();
    config.provider_factory = [base_url = options.base_url,
                               model = options.model,
                               api_key = options.api_key]() {
        HttpProviderConfig provider_config;
        provider_config.base_url = base_url;
        provider_config.model = model;
        provider_config.api_key = api_key;
        return std::make_unique<HttpProvider>(provider_config);
    };
    return config;
}

std::string prompt_for_session(ProjectSession& session) {
    const auto snapshot = session.snapshot();
    return "[" + snapshot.project_id + ":" + snapshot.session_id + ":"
        + snapshot.active_profile + ":" + snapshot.working_dir.string() + "]> ";
}

std::string prompt_for_session(ProjectSession& session,
                               const std::optional<std::string>& active_run_id) {
    if (!active_run_id.has_value()) {
        return prompt_for_session(session);
    }

    const auto snapshot = session.snapshot();
    return "[" + snapshot.project_id + ":" + snapshot.session_id + ":"
        + snapshot.active_profile + ":paused:" + *active_run_id + "]> ";
}

void print_help() {
    std::cout
        << "/help                show this help\n"
        << "/profile <name>      switch profiles\n"
        << "/tools               show visible tools for the current session\n"
        << "/sessions            list sessions\n"
        << "/runs                list persisted runs\n"
        << "/inspect run [id]    inspect the current or named run\n"
        << "/use <session-id>    attach to another session\n"
        << "/reset               clear the current session history\n"
        << "/clear               clear the terminal\n"
        << "/cwd [path]          show or reopen the session at a different working dir\n"
        << "/model [name]        show or reopen the host with a different model\n"
        << "/resume [decision]   resume a paused run (default: approve)\n"
        << "/stop [run-id]       stop the paused run\n"
        << "/cancel [run-id]     cancel the paused run\n"
        << "/quit                exit the CLI\n";
}

void print_tools(ProjectSession& session) {
    for (const auto& tool : session.tools()) {
        std::vector<std::string> tags;
        if (tool.read_only) {
            tags.push_back("read_only");
        }
        if (tool.destructive) {
            tags.push_back("destructive");
        }
        if (tool.shell) {
            tags.push_back("shell");
        }
        if (tool.network) {
            tags.push_back("network");
        }
        if (tool.mcp) {
            tags.push_back("mcp");
        }
        if (tool.sub_agent) {
            tags.push_back("sub_agent");
        }

        std::cout << tool.name;
        if (!tags.empty()) {
            std::cout << " [";
            for (std::size_t index = 0; index < tags.size(); ++index) {
                if (index > 0) {
                    std::cout << ", ";
                }
                std::cout << tags[index];
            }
            std::cout << "]";
        }
        std::cout << '\n';
    }
}

void print_runs(ProjectEngineHost& host) {
    for (const auto& summary : host.list_runs()) {
        std::cout << summary.run_id << "  " << run_status_name(summary.status)
                  << "  " << summary.profile << "  session=" << summary.session_id << '\n';
    }
}

void print_run(ProjectEngineHost& host, const std::string& run_id) {
    const auto result = host.get_run(run_id);
    if (!result.has_value()) {
        throw std::runtime_error("run not found: " + run_id);
    }
    print_json(run_result_json(*result));
}

void submit_and_wait(ProjectEngineHost& host,
                     ProjectSession& session,
                     const std::string& input,
                     TerminalObserver& observer,
                     ApprovalDelegate& approvals,
                     std::optional<std::string>& active_run_id) {
    auto run = session.submit_turn(input, observer, approvals);
    active_run_id = run->run_id();
    const std::string run_id = *active_run_id;

    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); });
    if (outcome == RunWaitOutcome::Paused) {
        observer.finish_waiting();
        std::cout << "\nrun paused: " << run_id << '\n';
        return;
    }

    run->wait();

    if (const auto result = host.get_run(*active_run_id); result.has_value()) {
        if (result->status != RunStatus::Completed && result->error.has_value()) {
            std::cerr << "\nerror: " << *result->error << '\n';
        }
    }

    observer.finish_waiting();
    active_run_id.reset();
    std::cout << '\n';
}

void resolve_paused_run(ProjectEngineHost& host,
                        const std::string& run_id,
                        const std::string& decision,
                        TerminalObserver& observer,
                        std::optional<std::string>& active_run_id) {
    if (!host.resume_run(run_id, decision)) {
        throw std::runtime_error("paused run not found: " + run_id);
    }

    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); });
    if (outcome == RunWaitOutcome::Paused) {
        observer.finish_waiting();
        std::cout << "run paused again: " << run_id << '\n';
        active_run_id = run_id;
        return;
    }

    observer.finish_waiting();
    active_run_id.reset();
    if (const auto result = host.get_run(run_id); result.has_value()) {
        print_json(run_result_json(*result));
    }
}

std::string require_run_id(const std::optional<std::string>& active_run_id,
                           std::istringstream& stream) {
    std::string run_id;
    stream >> run_id;
    if (!run_id.empty()) {
        return run_id;
    }
    if (active_run_id.has_value()) {
        return *active_run_id;
    }
    throw std::runtime_error("run id required");
}

int run_once(ProjectEngineHost& host, const CliOptions& options) {
    TerminalObserver observer;
    TerminalApprovals approvals;

    SessionOptions session_options;
    session_options.profile = options.profile;

    auto session = options.session_id
        ? host.resume_session(*options.session_id)
        : host.open_session(session_options);

    if (!options.prompt.has_value()) {
        throw std::invalid_argument("run mode requires --prompt");
    }

    auto run = session->submit_turn(*options.prompt, observer, approvals);
    const std::string run_id = run->run_id();
    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); });
    if (outcome != RunWaitOutcome::Paused) {
        run->wait();
    }
    observer.finish_waiting();
    const auto result = run->result();
    std::cout << '\n';
    print_json(run_result_json(result));
    return result.status == RunStatus::Completed ? 0 : 1;
}

int run_inspect(ProjectEngineHost& host, const CliOptions& options) {
    if (options.run_id.has_value()) {
        print_run(host, *options.run_id);
        return 0;
    }

    if (options.session_id.has_value()) {
        auto session = host.resume_session(*options.session_id);
        print_json(session_snapshot_json(session->snapshot(), session->tools()));
        return 0;
    }

    json payload;
    payload["sessions"] = json::array();
    for (const auto& summary : host.list_sessions()) {
        payload["sessions"].push_back(session_summary_json(summary));
    }
    payload["runs"] = json::array();
    for (const auto& summary : host.list_runs()) {
        payload["runs"].push_back(run_summary_json(summary));
    }
    print_json(payload);
    return 0;
}

int run_resume(ProjectEngineHost& host, const CliOptions& options) {
    if (!options.run_id.has_value()) {
        throw std::invalid_argument("resume mode requires --run-id");
    }

    if (!host.resume_run(*options.run_id, options.resume_input)) {
        std::cerr << "resume failed for run " << *options.run_id
                  << "; only live paused runs can be resumed in the current process\n";
        return 1;
    }

    TerminalObserver observer;
    const auto outcome = wait_for_run_block_or_finish(
        host,
        *options.run_id,
        [&observer, run_id = *options.run_id]() { observer.note_wait_tick(run_id); });
    observer.finish_waiting();
    const auto result = host.get_run(*options.run_id);
    if (result.has_value()) {
        print_json(run_result_json(*result));
    }
    return outcome == RunWaitOutcome::Finished ? 0 : 1;
}

int run_repl(ProjectEngineHost& host, const CliOptions& options) {
    CliOptions state = options;
    TerminalObserver observer;
    PauseForResumeApprovals approvals;

    SessionOptions session_options;
    session_options.profile = state.profile;

    auto session = state.session_id
        ? host.resume_session(*state.session_id)
        : host.open_session(session_options);
    std::optional<std::string> active_run_id;

    for (;;) {
        std::cout << prompt_for_session(*session, active_run_id) << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        if (line.empty()) {
            continue;
        }

        if (line[0] == '/') {
            std::istringstream stream(line.substr(1));
            std::string command;
            stream >> command;

            if (command == "quit") {
                return 0;
            }
            if (command == "help") {
                print_help();
                continue;
            }
            if (command == "reset") {
                if (active_run_id.has_value()) {
                    std::cerr << "resolve the paused run before resetting the session\n";
                    continue;
                }
                session->reset();
                continue;
            }
            if (command == "sessions") {
                for (const auto& summary : host.list_sessions()) {
                    std::cout << summary.session_id << "  " << summary.message_count
                              << " messages  " << summary.active_profile << '\n';
                }
                continue;
            }
            if (command == "tools") {
                print_tools(*session);
                continue;
            }
            if (command == "runs") {
                print_runs(host);
                continue;
            }
            if (command == "inspect") {
                std::string subject;
                stream >> subject;
                if (subject != "run") {
                    std::cerr << "usage: /inspect run [id]\n";
                    continue;
                }
                try {
                    print_run(host, require_run_id(active_run_id, stream));
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "use") {
                if (active_run_id.has_value()) {
                    std::cerr << "resolve the paused run before switching sessions\n";
                    continue;
                }
                std::string target_session;
                stream >> target_session;
                if (target_session.empty()) {
                    std::cerr << "session id required\n";
                    continue;
                }
                session->close();
                session = host.resume_session(target_session);
                state.session_id = target_session;
                continue;
            }
            if (command == "profile") {
                if (active_run_id.has_value()) {
                    std::cerr << "resolve the paused run before switching profiles\n";
                    continue;
                }
                std::string profile;
                stream >> profile;
                if (profile.empty()) {
                    std::cerr << "profile name required\n";
                    continue;
                }
                session->set_profile(profile);
                state.profile = profile;
                continue;
            }
            if (command == "clear") {
                std::cout << "\x1b[2J\x1b[H" << std::flush;
                continue;
            }
            if (command == "cwd") {
                std::string cwd;
                stream >> cwd;
                if (cwd.empty()) {
                    std::cout << session->snapshot().working_dir.string() << '\n';
                    continue;
                }
                if (active_run_id.has_value()) {
                    std::cerr << "resolve the paused run before changing the working directory\n";
                    continue;
                }
                const auto profile_name = session->active_profile();
                session->close();
                state.working_dir = std::filesystem::path(cwd);
                state.session_id.reset();
                session = host.open_session(SessionOptions{
                    .profile = profile_name,
                    .session_id = std::nullopt,
                    .working_dir_override = state.working_dir,
                });
                continue;
            }
            if (command == "model") {
                std::string model;
                stream >> model;
                if (model.empty()) {
                    std::cout << state.model << '\n';
                    continue;
                }
                if (active_run_id.has_value()) {
                    std::cerr << "resolve the paused run before changing models\n";
                    continue;
                }
                state.model = model;
                session->close();
                host.reload(build_config(state));
                state.session_id.reset();
                session = host.open_session(SessionOptions{
                    .profile = state.profile,
                    .session_id = std::nullopt,
                    .working_dir_override = state.working_dir,
                });
                continue;
            }
            if (command == "resume") {
                try {
                    const std::string decision = [&]() {
                        std::string value;
                        stream >> value;
                        return value.empty() ? std::string{"approve"} : value;
                    }();
                    resolve_paused_run(host, require_run_id(active_run_id, stream), decision, observer, active_run_id);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "cancel") {
                try {
                    const auto run_id = require_run_id(active_run_id, stream);
                    if (!host.cancel_run(run_id)) {
                        std::cerr << "run not found: " << run_id << '\n';
                        continue;
                    }
                    (void)wait_for_run_block_or_finish(host, run_id);
                    active_run_id.reset();
                    print_run(host, run_id);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "stop") {
                try {
                    const auto run_id = require_run_id(active_run_id, stream);
                    if (!host.stop_run(run_id)) {
                        std::cerr << "run not found: " << run_id << '\n';
                        continue;
                    }
                    (void)wait_for_run_block_or_finish(host, run_id);
                    active_run_id.reset();
                    print_run(host, run_id);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }

            std::cerr << "unknown command: " << command << '\n';
            continue;
        }

        if (active_run_id.has_value()) {
            std::cerr << "resolve the paused run before submitting another turn\n";
            continue;
        }

        submit_and_wait(host, *session, line, observer, approvals, active_run_id);
    }
}

}  // namespace

std::string usage_text(const char* program_name) {
    std::ostringstream stream;
    stream
        << "Usage: " << program_name << " [repl|run|inspect|resume] --project-id <id> --workspace-root <path>"
        << " --base-url <url> --model <model> [options]\n"
        << "Options:\n"
        << "  --cwd <path>         Override the working directory within the workspace\n"
        << "  --storage-dir <path> Persist sessions and runs under this directory\n"
        << "  --profile <name>     Session profile (default: explore)\n"
        << "  --session-id <id>    Resume an existing session\n"
        << "  --run-id <id>        Inspect or resume a specific run\n"
        << "  --prompt <text>      Prompt to run in run mode\n"
        << "  --resume-input <v>   Resume decision for resume mode (default: approve)\n"
        << "  --api-key <value>    Provider API key\n"
        << "  --help               Show this message\n";
    return stream.str();
}

int run_cli(const CliOptions& options) {
    auto host = ProjectEngineHost::create(build_config(options));
    if (options.mode == "run") {
        return run_once(*host, options);
    }
    if (options.mode == "inspect") {
        return run_inspect(*host, options);
    }
    if (options.mode == "resume") {
        return run_resume(*host, options);
    }
    return run_repl(*host, options);
}

}  // namespace omni::engine::cli