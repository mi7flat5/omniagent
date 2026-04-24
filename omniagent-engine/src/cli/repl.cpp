#include "cli/repl.h"
#include "cli/repl_internal.h"
#include "project_runtime_internal.h"

#include <omni/approval.h>
#include <omni/event.h>
#include <omni/host.h>
#include <omni/observer.h>
#include <omni/project_session.h>
#include <omni/run.h>

#include "providers/http_provider.h"
#include "tools/workspace_tools.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif

namespace omni::engine::cli {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;
using detail::CliApprovalPolicy;

volatile std::sig_atomic_t g_cli_interrupt_requests = 0;

void cli_sigint_handler(int) {
    const auto pending = g_cli_interrupt_requests;
    if (pending < 2) {
        g_cli_interrupt_requests = static_cast<std::sig_atomic_t>(pending + 1);
    }
}

int take_cli_interrupt_requests() {
    const int pending = static_cast<int>(g_cli_interrupt_requests);
    g_cli_interrupt_requests = 0;
    return pending;
}

class ScopedCliSignalHandler {
public:
    ScopedCliSignalHandler()
        : previous_sigint_(std::signal(SIGINT, cli_sigint_handler)) {
        g_cli_interrupt_requests = 0;
    }

    ~ScopedCliSignalHandler() {
        std::signal(SIGINT, previous_sigint_);
        g_cli_interrupt_requests = 0;
    }

private:
    using SignalHandler = void (*)(int);
    SignalHandler previous_sigint_ = SIG_DFL;
};

struct WorkflowSmokeExample {
    const char* profile;
    const char* prompt;
};

constexpr std::array<WorkflowSmokeExample, 4> kWorkflowSmokeExamples{{
    {"feature", "Add a JSON export command and tests"},
    {"refactor", "Extract run formatting without changing behavior"},
    {"audit", "Review approval handling for operator-facing risks"},
    {"bugfix", "Fix paused-run resume and add a regression test"},
}};

std::filesystem::path default_storage_dir(const CliOptions& options) {
    return options.workspace_root / ".omniagent" / "engine-cli";
}

CliApprovalPolicy parse_approval_policy(const CliOptions& options,
                                        CliApprovalPolicy default_policy) {
    return detail::parse_approval_policy_value(options.approval_policy, default_policy);
}

std::string supported_profile_names_text() {
    const auto profiles = default_profiles();

    std::ostringstream stream;
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << profiles[index].name;
    }
    return stream.str();
}

void append_workflow_help(std::ostringstream& stream,
                          const std::string& profiles_prefix,
                          const std::string& examples_prefix,
                          const std::string& example_lead) {
    stream << profiles_prefix << supported_profile_names_text() << '\n';
    stream << examples_prefix << '\n';
    for (const auto& example : kWorkflowSmokeExamples) {
        stream << example_lead << example.profile << " \"" << example.prompt << "\"\n";
    }
}

}  // namespace

namespace {

enum class ReplLineReadStatus {
    Submitted,
    Eof,
    Interrupted,
};

struct ReplLineReadResult {
    ReplLineReadStatus status = ReplLineReadStatus::Submitted;
    std::string line;
};

ReplLineReadResult read_repl_line_with_getline(const std::string& prompt) {
    std::cout << prompt << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
        if (std::cin.fail() && !std::cin.eof()) {
            std::cin.clear();
            std::cout << '\n';
            return {ReplLineReadStatus::Interrupted, {}};
        }
        std::cout << '\n';
        return {ReplLineReadStatus::Eof, {}};
    }

    return {ReplLineReadStatus::Submitted, std::move(line)};
}

#if !defined(_WIN32)
class ScopedRawTerminal {
public:
    ScopedRawTerminal() {
        if (::tcgetattr(STDIN_FILENO, &original_) != 0) {
            return;
        }

        termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            enabled_ = true;
        }
    }

    ~ScopedRawTerminal() {
        if (enabled_) {
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
    }

    bool enabled() const {
        return enabled_;
    }

private:
    termios original_{};
    bool enabled_ = false;
};

enum class TerminalByteStatus {
    Ok,
    Eof,
    Interrupted,
};

struct TerminalByteReadResult {
    TerminalByteStatus status = TerminalByteStatus::Ok;
    unsigned char byte = 0;
};

TerminalByteReadResult read_terminal_byte() {
    unsigned char byte = 0;
    const ssize_t bytes_read = ::read(STDIN_FILENO, &byte, 1);
    if (bytes_read == 0) {
        return {TerminalByteStatus::Eof, 0};
    }
    if (bytes_read < 0) {
        if (errno == EINTR) {
            return {TerminalByteStatus::Interrupted, 0};
        }
        throw std::system_error(errno, std::generic_category(), "read terminal input");
    }
    return {TerminalByteStatus::Ok, byte};
}

struct EscapeSequenceReadResult {
    ReplLineReadStatus status = ReplLineReadStatus::Submitted;
    std::string sequence;
};

EscapeSequenceReadResult read_terminal_escape_sequence() {
    const auto first = read_terminal_byte();
    if (first.status == TerminalByteStatus::Interrupted) {
        return {ReplLineReadStatus::Interrupted, {}};
    }
    if (first.status == TerminalByteStatus::Eof) {
        return {ReplLineReadStatus::Eof, {}};
    }

    std::string sequence;
    sequence.push_back(static_cast<char>(first.byte));
    if (first.byte != '[' && first.byte != 'O') {
        return {ReplLineReadStatus::Submitted, std::move(sequence)};
    }

    while (sequence.size() < 8) {
        const auto next = read_terminal_byte();
        if (next.status == TerminalByteStatus::Interrupted) {
            return {ReplLineReadStatus::Interrupted, {}};
        }
        if (next.status == TerminalByteStatus::Eof) {
            return {ReplLineReadStatus::Eof, {}};
        }

        sequence.push_back(static_cast<char>(next.byte));
        if (sequence.front() == '[') {
            if (std::isalpha(next.byte) || next.byte == '~') {
                break;
            }
            continue;
        }
        if (std::isalpha(next.byte)) {
            break;
        }
    }

    return {ReplLineReadStatus::Submitted, std::move(sequence)};
}

void redraw_repl_input(const std::string& prompt,
                       const std::string& line,
                       std::size_t cursor) {
    cursor = std::min(cursor, line.size());
    std::cout << '\r' << "\x1b[2K" << prompt << line;
    const std::size_t tail_chars = line.size() - cursor;
    if (tail_chars > 0) {
        std::cout << "\x1b[" << tail_chars << 'D';
    }
    std::cout << std::flush;
}

ReplLineReadResult read_repl_line_with_history(const std::string& prompt,
                                               detail::ReplInputHistory& history) {
    if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
        return read_repl_line_with_getline(prompt);
    }

    ScopedRawTerminal raw_terminal;
    if (!raw_terminal.enabled()) {
        return read_repl_line_with_getline(prompt);
    }

    std::string line;
    std::size_t cursor = 0;
    redraw_repl_input(prompt, line, cursor);

    for (;;) {
        const auto read = read_terminal_byte();
        if (read.status == TerminalByteStatus::Interrupted) {
            std::cout << "\r\n" << std::flush;
            return {ReplLineReadStatus::Interrupted, {}};
        }
        if (read.status == TerminalByteStatus::Eof) {
            std::cout << "\r\n" << std::flush;
            return {ReplLineReadStatus::Eof, {}};
        }

        switch (read.byte) {
            case '\r':
            case '\n':
                std::cout << "\r\n" << std::flush;
                return {ReplLineReadStatus::Submitted, std::move(line)};
            case 1:
                cursor = 0;
                redraw_repl_input(prompt, line, cursor);
                break;
            case 4:
                if (line.empty()) {
                    std::cout << "\r\n" << std::flush;
                    return {ReplLineReadStatus::Eof, {}};
                }
                if (cursor < line.size()) {
                    line.erase(cursor, 1);
                    redraw_repl_input(prompt, line, cursor);
                }
                break;
            case 5:
                cursor = line.size();
                redraw_repl_input(prompt, line, cursor);
                break;
            case 8:
            case 127:
                if (cursor > 0) {
                    line.erase(cursor - 1, 1);
                    --cursor;
                    redraw_repl_input(prompt, line, cursor);
                }
                break;
            case '\x1b': {
                const auto escape = read_terminal_escape_sequence();
                if (escape.status == ReplLineReadStatus::Interrupted) {
                    std::cout << "\r\n" << std::flush;
                    return {ReplLineReadStatus::Interrupted, {}};
                }
                if (escape.status == ReplLineReadStatus::Eof) {
                    std::cout << "\r\n" << std::flush;
                    return {ReplLineReadStatus::Eof, {}};
                }

                if (escape.sequence == "[A") {
                    if (const auto previous = detail::previous_repl_history_entry(history, line);
                        previous.has_value()) {
                        line = *previous;
                        cursor = line.size();
                        redraw_repl_input(prompt, line, cursor);
                    }
                    break;
                }
                if (escape.sequence == "[B") {
                    if (const auto next = detail::next_repl_history_entry(history);
                        next.has_value()) {
                        line = *next;
                        cursor = line.size();
                        redraw_repl_input(prompt, line, cursor);
                    }
                    break;
                }
                if (escape.sequence == "[C") {
                    if (cursor < line.size()) {
                        ++cursor;
                        redraw_repl_input(prompt, line, cursor);
                    }
                    break;
                }
                if (escape.sequence == "[D") {
                    if (cursor > 0) {
                        --cursor;
                        redraw_repl_input(prompt, line, cursor);
                    }
                    break;
                }
                if (escape.sequence == "[H" || escape.sequence == "OH") {
                    cursor = 0;
                    redraw_repl_input(prompt, line, cursor);
                    break;
                }
                if (escape.sequence == "[F" || escape.sequence == "OF") {
                    cursor = line.size();
                    redraw_repl_input(prompt, line, cursor);
                    break;
                }
                if (escape.sequence == "[3~") {
                    if (cursor < line.size()) {
                        line.erase(cursor, 1);
                        redraw_repl_input(prompt, line, cursor);
                    }
                    break;
                }
                if (line.empty()) {
                    std::cout << "\r\n" << std::flush;
                    return {ReplLineReadStatus::Submitted, std::string{"\x1b"} + escape.sequence};
                }
                break;
            }
            default:
                if (read.byte < 32 || read.byte == '\t') {
                    break;
                }
                line.insert(line.begin() + static_cast<std::ptrdiff_t>(cursor),
                            static_cast<char>(read.byte));
                ++cursor;
                redraw_repl_input(prompt, line, cursor);
                break;
        }
    }
}
#endif

ReplLineReadResult read_repl_line(const std::string& prompt,
                                  detail::ReplInputHistory& history) {
#if defined(_WIN32)
    (void)history;
    return read_repl_line_with_getline(prompt);
#else
    return read_repl_line_with_history(prompt, history);
#endif
}

void print_approval_request(const std::string& tool_name,
                            const nlohmann::json& args,
                            const std::string& description) {
    std::cout << "\napproval required for tool '" << tool_name << "'\n";
    std::cout << description << '\n';
    if (!args.is_null()) {
        std::cout << args.dump(2) << '\n';
    }
}

ApprovalDecision prompt_for_approval(const std::string& tool_name,
                                     const nlohmann::json& args,
                                     const std::string& description) {
    print_approval_request(tool_name, args, description);
    std::cout << "approve? [y/N/a] " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
        return ApprovalDecision::Deny;
    }
    if (line.empty()) {
        return ApprovalDecision::Deny;
    }
    if (line[0] == 'a' || line[0] == 'A') {
        return ApprovalDecision::ApproveAlways;
    }
    if (line[0] == 'y' || line[0] == 'Y') {
        return ApprovalDecision::Approve;
    }
    return ApprovalDecision::Deny;
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
        {"pending_clarification", nullptr},
    };

    if (result.pending_approval.has_value()) {
        payload["pending_approval"] = {
            {"tool_name", result.pending_approval->tool_name},
            {"args", result.pending_approval->args},
            {"description", result.pending_approval->description},
        };
    }
    if (result.pending_clarification.has_value()) {
        json questions = json::array();
        for (const auto& question : result.pending_clarification->questions) {
            questions.push_back({
                {"id", question.id},
                {"stage", question.stage},
                {"severity", question.severity},
                {"quote", question.quote},
                {"question", question.question},
                {"recommended_default", question.recommended_default},
                {"answer_type", question.answer_type},
                {"options", question.options},
            });
        }
        payload["pending_clarification"] = {
            {"tool_name", result.pending_clarification->tool_name},
            {"clarification_mode", result.pending_clarification->clarification_mode},
            {"clarification_message", result.pending_clarification->clarification_message},
            {"pending_question_ids", result.pending_clarification->pending_question_ids},
            {"questions", std::move(questions)},
            {"raw_payload", result.pending_clarification->raw_payload},
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
                                            std::function<void(int)> on_interrupt = {},
                                            std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    const bool has_timeout = timeout.count() > 0;
    const auto deadline = has_timeout
        ? std::chrono::steady_clock::now() + timeout
        : std::chrono::steady_clock::time_point::max();
    int handled_interrupts = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const int pending_interrupts = take_cli_interrupt_requests();
        for (int index = 0; index < pending_interrupts; ++index) {
            ++handled_interrupts;
            if (on_interrupt) {
                on_interrupt(handled_interrupts);
            }
        }

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
            last_activity_at_ = wait_started_at_;
            last_status_at_ = wait_started_at_;
            printed_thinking_ = false;
        } else if (const auto* text = std::get_if<TextDeltaEvent>(&event)) {
            note_activity_locked();
            flush_status_line_locked();
            std::cout << text->text << std::flush;
        } else if (std::holds_alternative<ThinkingDeltaEvent>(event)) {
            note_activity_locked();
            if (!printed_thinking_) {
                printed_thinking_ = true;
                last_status_at_ = last_activity_at_;
                print_status_locked(detail::format_activity_status());
            }
        } else if (const auto* error = std::get_if<ErrorEvent>(&event)) {
            note_activity_locked();
            flush_status_line_locked();
            std::cerr << "\nerror: " << error->message << '\n';
        } else if (const auto* tool = std::get_if<ToolUseStartEvent>(&event)) {
            note_activity_locked();
            print_status_locked(
                detail::format_activity_status(detail::tool_indicator_text(tool->name,
                                                                           tool->input)));
        } else if (const auto* clarification = std::get_if<ClarificationRequestedEvent>(&event)) {
            note_activity_locked();
            flush_status_line_locked();
            RunResult result;
            result.run_id = clarification->context.run_id;
            result.pending_clarification = clarification->clarification;
            pending_clarification_ = detail::pending_clarification_state_from_run(result);
            if (pending_clarification_.has_value()) {
                std::cout << "\n[clarification required] "
                          << pending_clarification_->questions.size()
                          << " question(s) pending\n" << std::flush;
            }
        } else if (const auto* agent = std::get_if<AgentSpawnedEvent>(&event)) {
            note_activity_locked();
            print_status_locked(
                detail::format_activity_status(detail::agent_indicator_text(agent->profile)));
        } else if (const auto* agent = std::get_if<AgentCompletedEvent>(&event)) {
            note_activity_locked();
            print_status_locked(
                detail::format_activity_status(
                    detail::agent_completion_indicator_text(agent->success)));
        } else if (std::get_if<RunPausedEvent>(&event) != nullptr) {
            note_activity_locked();
            print_status_locked(detail::run_paused_indicator_text());
        }
    }

    void note_wait_tick(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (active_run_id_.empty()) {
            active_run_id_ = run_id;
            wait_started_at_ = now;
            last_activity_at_ = now;
            last_status_at_ = now;
            printed_thinking_ = false;
            print_status_locked(detail::run_started_indicator_text());
            return;
        }
        if (now - last_status_at_ < 2s) {
            return;
        }
        if (last_activity_at_ != std::chrono::steady_clock::time_point{}
            && now - last_activity_at_ < 2s) {
            return;
        }
        if (!printed_thinking_) {
            printed_thinking_ = true;
            print_status_locked(detail::format_activity_status());
        } else {
            print_status_locked(detail::activity_tick_text());
        }
        last_status_at_ = now;
    }

    void finish_waiting() {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_status_line_locked();
        active_run_id_.clear();
        printed_thinking_ = false;
        wait_started_at_ = std::chrono::steady_clock::time_point{};
        last_activity_at_ = std::chrono::steady_clock::time_point{};
        last_status_at_ = std::chrono::steady_clock::time_point{};
    }

    std::optional<detail::PendingClarificationState> pending_clarification(
        const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_clarification_.has_value() || pending_clarification_->run_id != run_id) {
            return std::nullopt;
        }
        return pending_clarification_;
    }

    void print_notice(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_status_line_locked();
        std::cout << text << '\n' << std::flush;
    }

private:
    void print_status_locked(const std::string& text) {
        if (status_line_open_ && status_line_text_ == text) {
            return;
        }

        const std::size_t previous_width = status_line_width_;
        status_line_open_ = true;
        std::cout << '\r' << text;
        if (text.size() < previous_width) {
            std::cout << std::string(previous_width - text.size(), ' ');
        }
        std::cout << std::flush;
        status_line_text_ = text;
        status_line_width_ = text.size();
    }

    void flush_status_line_locked() {
        if (!status_line_open_) {
            return;
        }
        std::cout << '\n' << std::flush;
        status_line_open_ = false;
        status_line_text_.clear();
        status_line_width_ = 0;
    }

    void note_activity_locked() {
        last_activity_at_ = std::chrono::steady_clock::now();
    }

    std::mutex mutex_;
    std::string active_run_id_;
    std::chrono::steady_clock::time_point wait_started_at_{};
    std::chrono::steady_clock::time_point last_activity_at_{};
    std::chrono::steady_clock::time_point last_status_at_{};
    bool printed_thinking_ = false;
    bool status_line_open_ = false;
    std::string status_line_text_;
    std::size_t status_line_width_ = 0;
    std::optional<detail::PendingClarificationState> pending_clarification_;
};

void request_interrupt_action(ProjectEngineHost& host,
                              const std::string& run_id,
                              TerminalObserver& observer,
                              int interrupt_count) {
    if (interrupt_count <= 1) {
        if (host.stop_run(run_id)) {
            observer.print_notice("[stop requested; press Ctrl-C again to cancel]");
        }
        return;
    }

    if (host.cancel_run(run_id)) {
        observer.print_notice("[cancel requested]");
    }
}

class PolicyApprovals : public ApprovalDelegate {
public:
    PolicyApprovals(std::unordered_set<std::string> auto_approved_tools,
                    CliApprovalPolicy policy)
        : auto_approved_tools_(std::move(auto_approved_tools))
        , policy_(policy) {}

    ApprovalDecision on_approval_requested(const std::string& tool_name,
                                           const nlohmann::json& args,
                                           const std::string& description) override {
        if (auto_approved_tools_.contains(tool_name)) {
            return ApprovalDecision::Approve;
        }

        if (policy_ == CliApprovalPolicy::Pause
            || policy_ == CliApprovalPolicy::AutoReadOnlyPause) {
            print_approval_request(tool_name, args, description);
            std::cout << "run paused; use /resume [approve|deny|approve_always], /stop, or /cancel\n";
            return ApprovalDecision::Pause;
        }

        return prompt_for_approval(tool_name, args, description);
    }

private:
    std::unordered_set<std::string> auto_approved_tools_;
    CliApprovalPolicy policy_;
};

std::unique_ptr<ApprovalDelegate> make_approval_delegate(const CliOptions& options,
                                                         ProjectSession& session,
                                                         CliApprovalPolicy default_policy) {
    const CliApprovalPolicy policy = parse_approval_policy(options, default_policy);
    return std::make_unique<PolicyApprovals>(
    detail::auto_approved_tool_names(session.tools(), policy),
        policy);
};

ProjectRuntimeConfig build_config(const CliOptions& options) {
    ProjectRuntimeConfig config;
    config.workspace.project_id = options.project_id;
    config.workspace.workspace_root = options.workspace_root;
    config.workspace.working_dir = options.working_dir.value_or(options.workspace_root);
    config.engine.session_storage_dir = options.storage_dir.value_or(default_storage_dir(options));
    config.engine.temperature = options.temperature;
    config.engine.top_p = options.top_p;
    config.engine.top_k = options.top_k;
    config.engine.min_p = options.min_p;
    config.engine.presence_penalty = options.presence_penalty;
    config.engine.frequency_penalty = options.frequency_penalty;
    if (options.max_tokens.has_value()) {
        config.engine.initial_max_tokens = *options.max_tokens;
    }
    config.project_tools = make_default_workspace_tools();
    config.provider_factory = [base_url = options.base_url,
                               model = options.model,
                               api_key = options.api_key,
                               max_context_tokens = options.max_context_tokens,
                               max_tokens = options.max_tokens]() {
        HttpProviderConfig provider_config;
        provider_config.base_url = base_url;
        provider_config.model = model;
        provider_config.api_key = api_key;
        if (max_context_tokens.has_value()) {
            provider_config.max_context_tokens = *max_context_tokens;
        }
        if (max_tokens.has_value()) {
            provider_config.max_output_tokens = *max_tokens;
        }
        return std::make_unique<HttpProvider>(provider_config);
    };
    return config;
}

std::string prompt_for_session(ProjectSession& session) {
    const auto snapshot = session.snapshot();
    return detail::format_project_prompt(snapshot.project_id);
}

std::string prompt_for_session(ProjectSession& session,
                               const std::optional<std::string>&,
                               const std::optional<detail::PendingClarificationState>&) {
    return prompt_for_session(session);
}

void print_help() {
    std::cout << repl_help_text();
}

void print_pending_clarifications(const std::optional<detail::PendingClarificationState>& state) {
    if (!state.has_value()) {
        std::cout << "no pending clarifications\n";
        return;
    }
    std::cout << detail::format_clarifications(*state);
}

void print_staged_clarification_answers(const std::optional<detail::PendingClarificationState>& state) {
    if (!state.has_value()) {
        std::cout << "no pending clarifications\n";
        return;
    }
    std::cout << detail::format_staged_answers(*state);
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
                     std::optional<std::string>& active_run_id,
                     std::optional<detail::PendingClarificationState>& pending_clarification) {
    auto run = session.submit_turn(input, observer, approvals);
    active_run_id = run->run_id();
    const std::string run_id = *active_run_id;

    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); },
        [&host, &observer, run_id](int interrupt_count) {
            request_interrupt_action(host, run_id, observer, interrupt_count);
        });
    if (outcome == RunWaitOutcome::Paused) {
        pending_clarification = observer.pending_clarification(run_id);
        if (!pending_clarification.has_value()) {
            if (const auto result = host.get_run(run_id); result.has_value()) {
                pending_clarification = detail::pending_clarification_state_from_run(*result);
            }
        }
        observer.finish_waiting();
        return;
    }

    run->wait();

    pending_clarification = observer.pending_clarification(run_id);
    if (!pending_clarification.has_value()) {
        if (const auto result = host.get_run(run_id); result.has_value()) {
            pending_clarification = detail::pending_clarification_state_from_run(*result);
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
                        std::optional<std::string>& active_run_id,
                        std::optional<detail::PendingClarificationState>& pending_clarification) {
    if (!host.resume_run(run_id, decision)) {
        throw std::runtime_error("paused run not found: " + run_id);
    }

    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); },
        [&host, &observer, run_id](int interrupt_count) {
            request_interrupt_action(host, run_id, observer, interrupt_count);
        });
    if (outcome == RunWaitOutcome::Paused) {
        pending_clarification = observer.pending_clarification(run_id);
        if (!pending_clarification.has_value()) {
            if (const auto result = host.get_run(run_id); result.has_value()) {
                pending_clarification = detail::pending_clarification_state_from_run(*result);
            }
        }
        observer.finish_waiting();
        active_run_id = run_id;
        return;
    }

    pending_clarification = observer.pending_clarification(run_id);
    if (!pending_clarification.has_value()) {
        if (const auto result = host.get_run(run_id); result.has_value()) {
            pending_clarification = detail::pending_clarification_state_from_run(*result);
        }
    }
    observer.finish_waiting();
    active_run_id.reset();
    if (const auto result = host.get_run(run_id); result.has_value()) {
        print_json(run_result_json(*result));
    }
}

std::string require_run_id(const std::optional<std::string>& active_run_id,
                           const std::optional<std::string>& session_run_id,
                           std::istringstream& stream) {
    std::string run_id;
    stream >> run_id;
    if (!run_id.empty()) {
        return run_id;
    }
    if (active_run_id.has_value()) {
        return *active_run_id;
    }
    if (session_run_id.has_value()) {
        return *session_run_id;
    }
    throw std::runtime_error("run id required");
}

std::optional<std::string> function_key_alias_command(const std::string& line) {
    if (line == "\x1b[17~") {
        return std::string{"/rewind 1"};
    }
    if (line == "\x1b[18~") {
        return std::string{"/fork"};
    }
    if (line == "\x1b[19~") {
        return std::string{"/stop"};
    }
    return std::nullopt;
}

int run_once(ProjectEngineHost& host, const CliOptions& options) {
    ScopedCliSignalHandler signal_scope;
    TerminalObserver observer;

    SessionOptions session_options;
    session_options.profile = options.profile;

    auto session = options.session_id
        ? host.resume_session(*options.session_id)
        : host.open_session(session_options);
    auto approvals = make_approval_delegate(options, *session, CliApprovalPolicy::Prompt);

    if (!options.prompt.has_value()) {
        throw std::invalid_argument("run mode requires --prompt");
    }

    auto run = session->submit_turn(*options.prompt, observer, *approvals);
    const std::string run_id = run->run_id();
    const auto outcome = wait_for_run_block_or_finish(
        host,
        run_id,
        [&observer, run_id]() { observer.note_wait_tick(run_id); },
        [&host, &observer, run_id](int interrupt_count) {
            request_interrupt_action(host, run_id, observer, interrupt_count);
        });
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
    ScopedCliSignalHandler signal_scope;
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
        [&observer, run_id = *options.run_id]() { observer.note_wait_tick(run_id); },
        [&host, &observer, run_id = *options.run_id](int interrupt_count) {
            request_interrupt_action(host, run_id, observer, interrupt_count);
        });
    observer.finish_waiting();
    const auto result = host.get_run(*options.run_id);
    if (result.has_value()) {
        print_json(run_result_json(*result));
    }
    return outcome == RunWaitOutcome::Finished ? 0 : 1;
}

int run_repl(ProjectEngineHost& host, const CliOptions& options) {
    ScopedCliSignalHandler signal_scope;
    CliOptions state = options;
    TerminalObserver observer;
    const auto history_storage_dir = state.storage_dir.value_or(default_storage_dir(state));

    SessionOptions session_options;
    session_options.profile = state.profile;

    auto session = state.session_id
        ? host.resume_session(*state.session_id)
        : host.open_session(session_options);
    auto approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
    std::optional<std::string> active_run_id;
    std::optional<detail::PendingClarificationState> pending_clarification;
    detail::ReplInputHistory input_history = detail::load_persisted_repl_history(history_storage_dir);
    bool printed_fn_key_fallback = false;
    bool printed_history_warning = false;

    for (;;) {
        take_cli_interrupt_requests();
        auto input = read_repl_line(prompt_for_session(*session, active_run_id, pending_clarification),
                                    input_history);
        if (input.status == ReplLineReadStatus::Interrupted) {
            continue;
        }
        if (input.status == ReplLineReadStatus::Eof) {
            return 0;
        }

        std::string line = std::move(input.line);

        if (const auto alias = function_key_alias_command(line); alias.has_value()) {
            line = *alias;
        } else if (!line.empty() && line.front() == '\x1b') {
            if (!printed_fn_key_fallback) {
                printed_fn_key_fallback = true;
                std::cout << "function-key aliases unavailable in this terminal; use /rewind, /fork, and /stop\n";
            }
            continue;
        }

        if (line.empty()) {
            continue;
        }

        detail::remember_repl_history_entry(input_history, line);
        try {
            detail::persist_repl_history(history_storage_dir, input_history);
        } catch (const std::exception& error) {
            if (!printed_history_warning) {
                printed_history_warning = true;
                std::cerr << "warning: " << error.what() << '\n';
            }
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
                if (active_run_id.has_value() || pending_clarification.has_value()) {
                    std::cerr << "resolve the paused run or pending clarifications before resetting the session\n";
                    continue;
                }
                session->reset();
                continue;
            }
            if (command == "rewind") {
                std::size_t count = 1;
                std::string count_text;
                stream >> count_text;
                if (!count_text.empty()) {
                    try {
                        std::size_t parsed_chars = 0;
                        unsigned long long parsed = std::stoull(count_text, &parsed_chars, 10);
                        if (parsed_chars != count_text.size() || parsed == 0) {
                            throw std::invalid_argument("invalid count");
                        }
                        count = static_cast<std::size_t>(parsed);
                    } catch (const std::exception&) {
                        std::cerr << "usage: /rewind [count]\n";
                        continue;
                    }
                }

                try {
                    const auto removed = session->rewind_messages(count);
                    if (removed == 0) {
                        std::cout << "rewind: no messages to remove\n";
                    } else {
                        const auto total = session->snapshot().messages.size();
                        std::cout << "rewind: removed " << removed
                                  << (removed == 1 ? " message" : " messages")
                                  << "; now " << total << " total\n";
                    }
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "fork") {
                std::string fork_session_id;
                stream >> fork_session_id;

                SessionOptions fork_options;
                fork_options.profile = session->active_profile();
                fork_options.working_dir_override = session->snapshot().working_dir;
                if (!fork_session_id.empty()) {
                    fork_options.session_id = fork_session_id;
                }

                try {
                    const std::string source_session_id = session->session_id();
                    session = host.fork_session(source_session_id, std::move(fork_options));
                    state.session_id = session->session_id();
                    state.profile = session->active_profile();
                    state.working_dir = session->snapshot().working_dir;
                    approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
                    active_run_id.reset();
                    pending_clarification.reset();
                    std::cout << "forked session " << source_session_id
                              << " -> " << session->session_id() << '\n';
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
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
                    print_run(host, require_run_id(active_run_id, session->snapshot().active_run_id, stream));
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "clarifications") {
                print_pending_clarifications(pending_clarification);
                continue;
            }
            if (command == "answer") {
                if (!pending_clarification.has_value()) {
                    std::cerr << "no pending clarifications\n";
                    continue;
                }

                std::string question_id;
                stream >> question_id;
                std::string value;
                std::getline(stream, value);
                value = detail::trim_copy(std::move(value));
                if (question_id.empty() || value.empty()) {
                    std::cerr << "usage: /answer <id> <text>\n";
                    continue;
                }
                if (const auto* question = detail::find_question(*pending_clarification, question_id);
                    question == nullptr) {
                    std::cerr << "unknown clarification id: " << question_id << '\n';
                    continue;
                }
                pending_clarification->staged_answers[question_id] = value;
                if (detail::is_delegate_phrase(value)) {
                    pending_clarification->delegate_unanswered = true;
                }
                std::cout << "staged answer for " << question_id << '\n';
                continue;
            }
            if (command == "answers") {
                print_staged_clarification_answers(pending_clarification);
                continue;
            }
            if (command == "use") {
                if (active_run_id.has_value() || pending_clarification.has_value()) {
                    std::cerr << "resolve the paused run or pending clarifications before switching sessions\n";
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
                approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
                pending_clarification.reset();
                continue;
            }
            if (command == "profile") {
                if (active_run_id.has_value() || pending_clarification.has_value()) {
                    std::cerr << "resolve the paused run or pending clarifications before switching profiles\n";
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
                approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
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
                if (active_run_id.has_value() || pending_clarification.has_value()) {
                    std::cerr << "resolve the paused run or pending clarifications before changing the working directory\n";
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
                approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
                pending_clarification.reset();
                continue;
            }
            if (command == "model") {
                std::string model;
                stream >> model;
                if (model.empty()) {
                    std::cout << state.model << '\n';
                    continue;
                }
                if (active_run_id.has_value() || pending_clarification.has_value()) {
                    std::cerr << "resolve the paused run or pending clarifications before changing models\n";
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
                approvals = make_approval_delegate(state, *session, CliApprovalPolicy::Pause);
                pending_clarification.reset();
                continue;
            }
            if (command == "resume") {
                if (pending_clarification.has_value()) {
                    std::cerr << "use /continue for clarification-paused runs\n";
                    continue;
                }
                try {
                    const std::string decision = [&]() {
                        std::string value;
                        stream >> value;
                        return value.empty() ? std::string{"approve"} : value;
                    }();
                    resolve_paused_run(host,
                                       require_run_id(active_run_id,
                                                      session->snapshot().active_run_id,
                                                      stream),
                                       decision,
                                       observer,
                                       active_run_id,
                                       pending_clarification);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "continue") {
                if (!pending_clarification.has_value()) {
                    std::cerr << "no pending clarifications\n";
                    continue;
                }

                std::string mode;
                stream >> mode;
                if (!mode.empty() && detail::is_delegate_phrase(mode)) {
                    pending_clarification->delegate_unanswered = true;
                }

                const std::string input = detail::build_clarification_resume_input(*pending_clarification);
                if (input.empty()) {
                    std::cerr << "stage at least one answer with /answer or use /continue delegate\n";
                    continue;
                }
                resolve_paused_run(host,
                                   pending_clarification->run_id,
                                   input,
                                   observer,
                                   active_run_id,
                                   pending_clarification);
                continue;
            }
            if (command == "cancel") {
                try {
                    const auto run_id = require_run_id(active_run_id,
                                                       session->snapshot().active_run_id,
                                                       stream);
                    if (!host.cancel_run(run_id)) {
                        std::cerr << "run not found: " << run_id << '\n';
                        continue;
                    }
                    (void)wait_for_run_block_or_finish(host, run_id);
                    active_run_id.reset();
                    pending_clarification.reset();
                    print_run(host, run_id);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }
            if (command == "stop") {
                try {
                    const auto run_id = require_run_id(active_run_id,
                                                       session->snapshot().active_run_id,
                                                       stream);
                    if (!host.stop_run(run_id)) {
                        std::cerr << "run not found: " << run_id << '\n';
                        continue;
                    }
                    (void)wait_for_run_block_or_finish(host, run_id);
                    active_run_id.reset();
                    pending_clarification.reset();
                    print_run(host, run_id);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                }
                continue;
            }

            std::cerr << "unknown command: " << command << '\n';
            continue;
        }

        if (active_run_id.has_value() && !pending_clarification.has_value()) {
            std::cerr << "resolve the paused run before submitting another turn\n";
            continue;
        }

        if (pending_clarification.has_value()) {
            std::string input = line;
            if (detail::is_delegate_phrase(line)) {
                pending_clarification->delegate_unanswered = true;
                const std::string delegated_input =
                    detail::build_clarification_resume_input(*pending_clarification);
                if (!delegated_input.empty()) {
                    input = delegated_input;
                }
            }
            resolve_paused_run(host,
                               pending_clarification->run_id,
                               input,
                               observer,
                               active_run_id,
                               pending_clarification);
            continue;
        }

        submit_and_wait(host,
                        *session,
                        line,
                        observer,
                        *approvals,
                        active_run_id,
                        pending_clarification);
    }
}

}  // namespace

std::string repl_help_text() {
    std::ostringstream stream;
    stream
        << "/help                show this help\n"
        << "/profile <name>      switch profiles\n"
        << "/tools               show visible tools for the current session\n"
        << "/sessions            list sessions\n"
        << "/runs                list persisted runs\n"
        << "/inspect run [id]    inspect the current or named run\n"
        << "/clarifications      show pending clarification questions\n"
        << "/answer <id> <text>  stage one clarification answer\n"
        << "/answers             show staged clarification answers\n"
        << "/use <session-id>    attach to another session\n"
        << "/reset               clear the current session history\n"
        << "/rewind [count]      remove one or more most recent messages\n"
        << "/fork [session-id]   clone current session history into a new session\n"
        << "/clear               clear the terminal\n"
        << "/cwd [path]          show or reopen the session at a different working dir\n"
        << "/model [name]        show or reopen the host with a different model\n"
        << "/resume [decision]   resume a paused run (default: approve)\n"
        << "/continue [delegate] resume the paused clarification run with staged answers\n"
        << "/stop [run-id]       stop the current or named run\n"
        << "/cancel [run-id]     cancel the current or named run\n"
        << "Up/Down arrows       traverse persisted input history\n"
        << "F6/F7/F8             aliases for /rewind 1, /fork, /stop\n"
        << "Ctrl-C               stop the active run; press again to cancel\n"
        << "/quit                exit the CLI\n";
    append_workflow_help(stream,
                         "Profiles: ",
                         "Workflow smoke prompts:",
                         "  /profile ");
    return stream.str();
}

std::string usage_text(const char* program_name) {
    std::ostringstream stream;
    stream
        << "Usage: " << program_name << " [repl|run|inspect|resume] --project-id <id> --workspace-root <path>"
        << " --base-url <url> --model <model> [options]\n"
        << "Options:\n"
        << "  --cwd <path>         Override the working directory within the workspace\n"
        << "  --storage-dir <path> Persist sessions and runs under this directory\n"
        << "  --profile <name>     Session profile (default: coordinator)\n"
        << "  --session-id <id>    Resume an existing session\n"
        << "  --run-id <id>        Inspect or resume a specific run\n"
        << "  --prompt <text>      Prompt to run in run mode\n"
        << "  --approval-policy <mode>  prompt, pause, auto-read-only, or auto-read-only-pause\n"
        << "  --resume-input <v>   Resume decision for resume mode (default: approve)\n"
        << "  --api-key <value>    Provider API key\n"
        << "  --max-context-tokens <n>  Context window hint/floor for compaction\n"
        << "  --temperature <n>    Provider temperature override\n"
        << "  --top-p <n>          Provider top-p override\n"
        << "  --top-k <n>          Local-provider top-k override\n"
        << "  --min-p <n>          Local-provider min-p override\n"
        << "  --presence-penalty <n>  Provider presence-penalty override\n"
        << "  --frequency-penalty <n> Provider frequency-penalty override\n"
        << "  --max-tokens <n>     Max output tokens per request\n"
        << "  Supported profiles: ";
    stream << supported_profile_names_text() << '\n';
    stream
        << "  Workflow smoke examples:\n";
    for (const auto& example : kWorkflowSmokeExamples) {
        stream << "    --profile " << example.profile << " --prompt \""
               << example.prompt << "\"\n";
    }
    stream
        << "  Delegated web research uses BRAVE_SEARCH_KEY when web_search is available.\n"
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