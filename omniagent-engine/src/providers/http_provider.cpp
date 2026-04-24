#include "http_provider.h"

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <omni/types.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct AsyncHttpRequestState {
    std::mutex                      mutex;
    std::condition_variable         cv;
    std::deque<std::string>         chunks;
    std::optional<httplib::Result>  result;
    std::shared_ptr<httplib::Client> client;
    std::exception_ptr              worker_error;
    bool                            finished = false;
    bool                            cancelled = false;
};

static std::runtime_error make_transport_error(const httplib::Result& result,
                                               const std::string& path) {
    return std::runtime_error(
        "HTTP transport failure for '" + path + "': " + httplib::to_string(result.error()));
}

static std::runtime_error make_http_status_error(const httplib::Response& response,
                                                 const std::string& path) {
    std::string message = "HTTP request to '" + path + "' failed with status "
        + std::to_string(response.status);
    if (!response.reason.empty()) {
        message += " (" + response.reason + ")";
    }
    if (!response.body.empty()) {
        message += ": " + response.body;
    }
    return std::runtime_error(message);
}

static std::string stop_reason_from_finish(const std::string& finish) {
    if (finish == "stop")       return "end_turn";
    if (finish == "tool_calls") return "tool_use";
    if (finish == "length")     return "max_tokens";
    return finish.empty() ? "" : finish;
}

static bool is_local_base_url(const std::string& base_url) {
    return base_url.find("localhost") != std::string::npos
        || base_url.find("127.0.0.1") != std::string::npos
        || base_url.find("192.168.") != std::string::npos;
}

// Parse "http(s)://host[:port]" out of a URL string, returning the rest as path.
static void split_url(const std::string& url,
                      std::string& scheme_host,  // "http://host:port"
                      std::string& path_prefix)  // "/v1" etc.
{
    // Find "://"
    const size_t sep = url.find("://");
    if (sep == std::string::npos) {
        scheme_host = url;
        path_prefix = "";
        return;
    }

    const std::string scheme = url.substr(0, sep + 3);  // "http://"
    const std::string rest   = url.substr(sep + 3);     // "host:port/path"

    const size_t slash = rest.find('/');
    if (slash == std::string::npos) {
        scheme_host = scheme + rest;
        path_prefix = "";
    } else {
        scheme_host = scheme + rest.substr(0, slash);
        path_prefix = rest.substr(slash);
    }
}

static std::string trim_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

static std::string strip_v1_suffix(std::string base_url) {
    base_url = trim_trailing_slashes(std::move(base_url));
    if (base_url.size() >= 3 && base_url.substr(base_url.size() - 3) == "/v1") {
        base_url.resize(base_url.size() - 3);
    }
    return base_url;
}

static int context_window_from_props(const nlohmann::json& body) {
    if (!body.contains("default_generation_settings")
        || !body["default_generation_settings"].is_object()) {
        return 0;
    }
    const auto& settings = body["default_generation_settings"];
    if (!settings.contains("n_ctx") || !settings["n_ctx"].is_number_integer()) {
        return 0;
    }
    const int context_window = settings["n_ctx"].get<int>();
    return context_window > 0 ? context_window : 0;
}

static int context_window_from_models(const nlohmann::json& body,
                                      const std::string& model_name) {
    if (!body.contains("data") || !body["data"].is_array() || body["data"].empty()) {
        return 0;
    }

    const nlohmann::json* selected = nullptr;
    for (const auto& item : body["data"]) {
        if (item.is_object() && item.value("id", std::string{}) == model_name) {
            selected = &item;
            break;
        }
    }
    if (!selected) {
        selected = &body["data"].front();
    }

    if (!selected->is_object() || !selected->contains("meta") || !(*selected)["meta"].is_object()) {
        return 0;
    }
    const auto& meta = (*selected)["meta"];
    if (!meta.contains("n_ctx_train") || !meta["n_ctx_train"].is_number_integer()) {
        return 0;
    }
    const int context_window = meta["n_ctx_train"].get<int>();
    return context_window > 0 ? context_window : 0;
}

static int context_window_from_ollama_show(const nlohmann::json& body) {
    if (!body.contains("model_info") || !body["model_info"].is_object()) {
        return 0;
    }

    for (const auto& [key, value] : body["model_info"].items()) {
        if (key.size() >= 15
            && key.substr(key.size() - 15) == ".context_length"
            && value.is_number_integer()) {
            const int context_window = value.get<int>();
            if (context_window > 0) {
                return context_window;
            }
        }
    }
    return 0;
}

static int detect_context_window(const HttpProviderConfig& config) {
    try {
        if (config.base_url.rfind("https://", 0) == 0) {
            return 0;
        }

        std::string scheme_host;
        std::string path_prefix;
        split_url(strip_v1_suffix(config.base_url), scheme_host, path_prefix);
        if (scheme_host.empty()) {
            return 0;
        }

        httplib::Client client(scheme_host);
        constexpr int detect_timeout_ms = 5000;
        client.set_connection_timeout(detect_timeout_ms / 1000,
                                      (detect_timeout_ms % 1000) * 1000);
        client.set_read_timeout(detect_timeout_ms / 1000,
                                (detect_timeout_ms % 1000) * 1000);
        client.set_write_timeout(detect_timeout_ms / 1000,
                                 (detect_timeout_ms % 1000) * 1000);

        httplib::Headers headers;
        if (!config.api_key.empty()) {
            headers.emplace("Authorization", "Bearer " + config.api_key);
        }

        if (auto res = client.Get(path_prefix + "/props", headers);
            res && res->status >= 200 && res->status < 300) {
            if (const int context_window = context_window_from_props(nlohmann::json::parse(res->body));
                context_window > 0) {
                return context_window;
            }
        }

        if (auto res = client.Get(path_prefix + "/v1/models", headers);
            res && res->status >= 200 && res->status < 300) {
            if (const int context_window =
                    context_window_from_models(nlohmann::json::parse(res->body), config.model);
                context_window > 0) {
                return context_window;
            }
        }

        nlohmann::json request_body;
        request_body["model"] = config.model;
        if (auto res = client.Post(path_prefix + "/api/show", headers,
                                   request_body.dump(), "application/json");
            res && res->status >= 200 && res->status < 300) {
            if (const int context_window = context_window_from_ollama_show(nlohmann::json::parse(res->body));
                context_window > 0) {
                return context_window;
            }
        }
    } catch (...) {
        return 0;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Constructor / capability accessors
// ---------------------------------------------------------------------------

HttpProvider::HttpProvider(HttpProviderConfig config)
    : config_(std::move(config))
{
    if (const int detected_context_window = detect_context_window(config_);
        detected_context_window > 0) {
        config_.max_context_tokens = std::max(config_.max_context_tokens,
                                              detected_context_window);
    }
}

ModelCapabilities HttpProvider::capabilities() const {
    return {
        .max_context_tokens     = config_.max_context_tokens,
        .max_output_tokens      = config_.max_output_tokens,
        .supports_tool_use      = true,
        .supports_thinking      = config_.supports_thinking,
        .supports_images        = config_.supports_images,
        .supports_cache_control = false,
    };
}

std::string HttpProvider::name() const {
    return "http/" + config_.model;
}

// ---------------------------------------------------------------------------
// Message conversion
// ---------------------------------------------------------------------------

nlohmann::json HttpProvider::message_to_openai(const Message& msg) const {
    if (msg.role == Role::ToolResult) {
        // One JSON message per tool result entry
        // (caller must expand multi-result messages before use)
        nlohmann::json arr = nlohmann::json::array();
        for (const ToolResult& tr : msg.tool_results) {
            arr.push_back({
                {"role",         "tool"},
                {"tool_call_id", tr.tool_use_id},
                {"content",      tr.content}
            });
        }
        return arr;  // array — caller flattens
    }

    std::string role_str;
    switch (msg.role) {
        case Role::User:      role_str = "user";      break;
        case Role::Assistant: role_str = "assistant"; break;
        case Role::System:    role_str = "system";    break;
        default:              role_str = "user";      break;
    }

    nlohmann::json out = {{"role", role_str}};

    // Collect text content and tool_calls separately
    std::string       text_content;
    nlohmann::json    tool_calls = nlohmann::json::array();

    for (const ContentBlock& blk : msg.content) {
        if (const auto* tc = std::get_if<TextContent>(&blk.data)) {
            text_content += tc->text;
        } else if (const auto* tuc = std::get_if<ToolUseContent>(&blk.data)) {
            tool_calls.push_back({
                {"id",   tuc->id},
                {"type", "function"},
                {"function", {
                    {"name",      tuc->name},
                    {"arguments", tuc->input.dump()}
                }}
            });
        }
        // ThinkingContent and ImageContent are not forwarded to OpenAI-compat APIs
    }

    if (!tool_calls.empty()) {
        out["tool_calls"] = tool_calls;
    }

    if (!text_content.empty()) {
        out["content"] = text_content;
    } else if (tool_calls.empty()) {
        out["content"] = "";
    }

    return out;
}

// ---------------------------------------------------------------------------
// Request body
// ---------------------------------------------------------------------------

nlohmann::json HttpProvider::build_request_body(const CompletionRequest& request) const {
    nlohmann::json body = {
        {"model",  config_.model},
        {"stream", true}
    };
    const bool is_local = is_local_base_url(config_.base_url);

    if (request.temperature.has_value()) {
        body["temperature"] = *request.temperature;
    }
    if (request.top_p.has_value()) {
        body["top_p"] = *request.top_p;
    }
    if (request.top_k.has_value() && is_local && *request.top_k > 0) {
        body["top_k"] = *request.top_k;
    }
    if (request.min_p.has_value() && is_local && *request.min_p > 0.0) {
        body["min_p"] = *request.min_p;
    }
    if (request.presence_penalty.has_value()) {
        body["presence_penalty"] = *request.presence_penalty;
    }
    if (request.frequency_penalty.has_value()) {
        body["frequency_penalty"] = *request.frequency_penalty;
    }
    if (request.max_tokens.has_value()) {
        body["max_tokens"] = *request.max_tokens;
    }
    if (!request.stop_sequences.empty()) {
        body["stop"] = request.stop_sequences;
    }

    // Build messages array
    nlohmann::json msgs = nlohmann::json::array();

    if (!request.system_prompt.empty()) {
        msgs.push_back({{"role", "system"}, {"content", request.system_prompt}});
    }

    for (const Message& msg : request.messages) {
        if (msg.role == Role::ToolResult) {
            // Expand: one OpenAI "tool" message per ToolResult entry
            for (const ToolResult& tr : msg.tool_results) {
                msgs.push_back({
                    {"role",         "tool"},
                    {"tool_call_id", tr.tool_use_id},
                    {"content",      tr.content}
                });
            }
        } else {
            msgs.push_back(message_to_openai(msg));
        }
    }

    body["messages"] = msgs;

    if (!request.tools.empty()) {
        body["tools"] = request.tools;
        if (request.tool_choice.has_value()) {
            body["tool_choice"] = *request.tool_choice;
        }
    }

    return body;
}

// ---------------------------------------------------------------------------
// SSE line parser
// Returns false when the stream is complete ("[DONE]").
// ---------------------------------------------------------------------------

bool HttpProvider::parse_sse_line(const std::string& data,
                                   StreamCallback& cb,
                                   bool& thinking_started,
                                   bool& thinking_open,
                                   bool& text_started,
                                   std::unordered_set<int>& open_tool_blocks,
                                   Usage& usage,
                                   bool& message_terminated) const
{
    if (data == "[DONE]") return false;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(data);
    } catch (...) {
        return true;  // malformed line — skip, keep going
    }

    // Extract finish_reason and delta
    std::string finish_reason;
    if (j.contains("choices") && !j["choices"].empty()) {
        const auto& choice = j["choices"][0];
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            finish_reason = choice["finish_reason"].get<std::string>();
        }
        if (choice.contains("delta")) {
            const auto& delta = choice["delta"];

            auto emit_thinking = [&](const std::string& text) {
                if (!text.empty()) {
                    if (!thinking_started) {
                        StreamEventData start_ev;
                        start_ev.type       = StreamEventType::ContentBlockStart;
                        start_ev.index      = 0;
                        start_ev.delta_type = "thinking";
                        cb(start_ev);
                        thinking_started = true;
                        thinking_open = true;
                    }
                    StreamEventData ev;
                    ev.type       = StreamEventType::ContentBlockDelta;
                    ev.index      = 0;
                    ev.delta_type = "thinking_delta";
                    ev.delta_text = text;
                    cb(ev);
                }
            };

            auto emit_text = [&](const std::string& text) {
                if (!text.empty()) {
                    if (thinking_open) {
                        StreamEventData stop_ev;
                        stop_ev.type  = StreamEventType::ContentBlockStop;
                        stop_ev.index = 0;
                        cb(stop_ev);
                        thinking_open = false;
                    }
                    if (!text_started) {
                        StreamEventData start_ev;
                        start_ev.type       = StreamEventType::ContentBlockStart;
                        start_ev.index      = thinking_started ? 1 : 0;
                        start_ev.delta_type = "text";
                        cb(start_ev);
                        text_started = true;
                    }
                    StreamEventData ev;
                    ev.type       = StreamEventType::ContentBlockDelta;
                    ev.index      = thinking_started ? 1 : 0;
                    ev.delta_type = "text_delta";
                    ev.delta_text = text;
                    cb(ev);
                }
            };

            if (delta.contains("content") && delta["content"].is_string()) {
                emit_text(delta["content"].get<std::string>());
            }
            if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                emit_thinking(delta["reasoning"].get<std::string>());
            }
            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                emit_thinking(delta["reasoning_content"].get<std::string>());
            }

            // Tool calls
            if (delta.contains("tool_calls")) {
                const int base_index = (thinking_started ? 1 : 0) + (text_started ? 1 : 0);
                for (const auto& tc : delta["tool_calls"]) {
                    const int tc_index = tc.value("index", 0);
                    const int abs_index = base_index + tc_index;

                    // New tool call?
                    if (tc.contains("id") && !tc["id"].is_null()) {
                        StreamEventData start_ev;
                        start_ev.type       = StreamEventType::ContentBlockStart;
                        start_ev.index      = abs_index;
                        start_ev.delta_type = "tool_use";
                        start_ev.tool_id    = tc["id"].get<std::string>();
                        if (tc.contains("function") && tc["function"].contains("name")) {
                            start_ev.tool_name = tc["function"]["name"].get<std::string>();
                        }
                        cb(start_ev);
                        open_tool_blocks.insert(abs_index);
                    }

                    // Argument fragment
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        const auto& args_value = tc["function"]["arguments"];
                        if (args_value.is_string()) {
                            const std::string args_frag = args_value.get<std::string>();
                            if (!args_frag.empty()) {
                                StreamEventData delta_ev;
                                delta_ev.type            = StreamEventType::ContentBlockDelta;
                                delta_ev.index           = abs_index;
                                delta_ev.delta_type      = "input_json_delta";
                                delta_ev.tool_input_delta = nlohmann::json(args_frag);
                                cb(delta_ev);
                            }
                        } else if (!args_value.is_null()) {
                            StreamEventData delta_ev;
                            delta_ev.type            = StreamEventType::ContentBlockDelta;
                            delta_ev.index           = abs_index;
                            delta_ev.delta_type      = "input_json_delta";
                            delta_ev.tool_input_delta = args_value;
                            cb(delta_ev);
                        }
                    }
                }
            }
        }
    }

    // Usage
    if (j.contains("usage")) {
        const auto& u = j["usage"];
        usage.input_tokens  = u.value("prompt_tokens",     0);
        usage.output_tokens = u.value("completion_tokens", 0);
    }

    // Finish reason — emit MessageDelta + MessageStop and signal done
    if (!finish_reason.empty()) {
        for (const int index : open_tool_blocks) {
            StreamEventData stop_ev;
            stop_ev.type  = StreamEventType::ContentBlockStop;
            stop_ev.index = index;
            cb(stop_ev);
        }
        open_tool_blocks.clear();

        StreamEventData md;
        md.type        = StreamEventType::MessageDelta;
        md.stop_reason = stop_reason_from_finish(finish_reason);
        md.usage       = usage;
        cb(md);

        StreamEventData ms;
        ms.type = StreamEventType::MessageStop;
        cb(ms);

        message_terminated = true;

        return false;  // signal done — prevent fallback from emitting duplicates
    }

    return true;
}

// ---------------------------------------------------------------------------
// Main complete() implementation
// ---------------------------------------------------------------------------

Usage HttpProvider::complete(const CompletionRequest& request,
                              StreamCallback           stream_cb,
                              std::atomic<bool>&       stop_flag)
{
    std::string scheme_host;
    std::string path_prefix;
    split_url(config_.base_url, scheme_host, path_prefix);

    const std::string post_path = path_prefix + "/chat/completions";

    httplib::Headers headers;
    if (!config_.api_key.empty()) {
        headers.emplace("Authorization", "Bearer " + config_.api_key);
    }

    // Emit MessageStart
    {
        StreamEventData ms;
        ms.type = StreamEventType::MessageStart;
        stream_cb(ms);
    }

    const nlohmann::json body = build_request_body(request);
    const std::string body_text = body.dump();

    std::string  line_buf;
    bool         thinking_started = false;
    bool         thinking_open = false;
    bool         text_started = false;
    std::unordered_set<int> open_tool_blocks;
    Usage        usage;
    bool         message_terminated = false;

    const auto finalize_stream = [&]() {
        if (thinking_open) {
            StreamEventData cbe;
            cbe.type  = StreamEventType::ContentBlockStop;
            cbe.index = 0;
            stream_cb(cbe);
            thinking_open = false;
        }

        if (text_started) {
            StreamEventData cbe;
            cbe.type  = StreamEventType::ContentBlockStop;
            cbe.index = thinking_started ? 1 : 0;
            stream_cb(cbe);
        }

        for (const int index : open_tool_blocks) {
            StreamEventData cbe;
            cbe.type  = StreamEventType::ContentBlockStop;
            cbe.index = index;
            stream_cb(cbe);
        }

        if (!message_terminated) {
            StreamEventData md;
            md.type        = StreamEventType::MessageDelta;
            md.stop_reason = "end_turn";
            md.usage       = usage;
            stream_cb(md);

            StreamEventData ms;
            ms.type = StreamEventType::MessageStop;
            stream_cb(ms);
        }
    };

    auto process_chunk = [&](const char* data, size_t len) -> bool {
        if (stop_flag.load(std::memory_order_relaxed)) return false;

        line_buf.append(data, len);

        // Process complete SSE lines
        size_t pos = 0;
        while (true) {
            const size_t nl = line_buf.find('\n', pos);
            if (nl == std::string::npos) break;

            std::string line = line_buf.substr(pos, nl - pos);
            pos = nl + 1;

            // Strip carriage return
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.rfind("data: ", 0) == 0) {
                const std::string payload = line.substr(6);
                if (!parse_sse_line(payload, stream_cb, thinking_started, thinking_open, text_started,
                                    open_tool_blocks, usage, message_terminated)) {
                    continue;
                }
            }

            if (stop_flag.load(std::memory_order_relaxed)) {
                line_buf.erase(0, pos);
                return false;
            }
        }

        line_buf.erase(0, pos);
        return true;
    };

    auto request_state = std::make_shared<AsyncHttpRequestState>();
    std::thread request_thread([request_state,
                                scheme_host,
                                post_path,
                                headers,
                                body_text,
                                connect_timeout_ms = config_.connect_timeout_ms,
                                read_timeout_ms = config_.read_timeout_ms]() mutable {
        try {
            auto client = std::make_shared<httplib::Client>(scheme_host);
            client->set_connection_timeout(connect_timeout_ms / 1000,
                                           (connect_timeout_ms % 1000) * 1000);
            client->set_read_timeout(read_timeout_ms / 1000,
                                     (read_timeout_ms % 1000) * 1000);
            client->set_write_timeout(30, 0);

            {
                std::lock_guard<std::mutex> lock(request_state->mutex);
                request_state->client = client;
                if (request_state->cancelled) {
                    request_state->client.reset();
                    request_state->finished = true;
                    request_state->cv.notify_all();
                    return;
                }
            }
            request_state->cv.notify_all();

            auto queueing_receiver = [request_state](const char* data, size_t len) -> bool {
                std::lock_guard<std::mutex> lock(request_state->mutex);
                if (request_state->cancelled) {
                    return false;
                }
                request_state->chunks.emplace_back(data, len);
                request_state->cv.notify_all();
                return true;
            };

            auto result = client->Post(post_path, headers, body_text, "application/json",
                                       queueing_receiver);

            {
                std::lock_guard<std::mutex> lock(request_state->mutex);
                request_state->result.emplace(std::move(result));
                request_state->client.reset();
                request_state->finished = true;
            }
            request_state->cv.notify_all();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(request_state->mutex);
                request_state->worker_error = std::current_exception();
                request_state->client.reset();
                request_state->finished = true;
            }
            request_state->cv.notify_all();
        }
    });

    const auto request_stop = [&]() {
        std::shared_ptr<httplib::Client> client;
        {
            std::lock_guard<std::mutex> lock(request_state->mutex);
            request_state->cancelled = true;
            client = request_state->client;
        }
        request_state->cv.notify_all();

        if (client) {
            client->stop();
            const auto sock = client->socket();
            if (sock != INVALID_SOCKET) {
                httplib::detail::shutdown_socket(sock);
            }
        }
    };

    const auto settle_request_thread = [&](bool allow_detach) {
        if (!request_thread.joinable()) {
            return;
        }

        if (!allow_detach) {
            request_thread.join();
            return;
        }

        {
            std::unique_lock<std::mutex> lock(request_state->mutex);
            if (!request_state->finished) {
                request_state->cv.wait_for(lock,
                                           std::chrono::milliseconds(100),
                                           [&]() { return request_state->finished; });
            }
        }

        if (request_state->finished) {
            request_thread.join();
        } else {
            request_thread.detach();
        }
    };

    while (true) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            request_stop();
            finalize_stream();
            settle_request_thread(true);
            return usage;
        }

        std::deque<std::string> chunks;
        bool finished = false;
        {
            std::unique_lock<std::mutex> lock(request_state->mutex);
            request_state->cv.wait_for(lock,
                                       std::chrono::milliseconds(25),
                                       [&]() {
                                           return !request_state->chunks.empty()
                                               || request_state->finished
                                               || stop_flag.load(std::memory_order_relaxed);
                                       });
            chunks.swap(request_state->chunks);
            finished = request_state->finished;
        }

        for (const auto& chunk : chunks) {
            if (!process_chunk(chunk.data(), chunk.size())) {
                break;
            }
        }

        if (finished) {
            break;
        }
    }

    settle_request_thread(false);

    std::exception_ptr worker_error;
    std::optional<httplib::Result> result;
    {
        std::lock_guard<std::mutex> lock(request_state->mutex);
        worker_error = request_state->worker_error;
        if (request_state->result.has_value()) {
            result.emplace(std::move(*request_state->result));
        }
    }

    if (worker_error) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            finalize_stream();
            return usage;
        }
        std::rethrow_exception(worker_error);
    }

    if (!result.has_value()) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            finalize_stream();
            return usage;
        }
        throw std::runtime_error("HTTP request to '" + post_path + "' did not produce a result");
    }

    if (!(*result)) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            finalize_stream();
            return usage;
        }
        throw make_transport_error(*result, post_path);
    }

    if ((*result)->status < 200 || (*result)->status >= 300) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            finalize_stream();
            return usage;
        }
        throw make_http_status_error(*(*result), post_path);
    }

    finalize_stream();

    return usage;
}

}  // namespace omni::engine
