#include "http_provider.h"

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <omni/types.h>

#include <sstream>
#include <stdexcept>
#include <string>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Constructor / capability accessors
// ---------------------------------------------------------------------------

HttpProvider::HttpProvider(HttpProviderConfig config)
    : config_(std::move(config))
{}

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

    if (request.temperature.has_value()) {
        body["temperature"] = *request.temperature;
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
                                   Usage& usage) const
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
                    }

                    // Argument fragment
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        const std::string args_frag =
                            tc["function"]["arguments"].get<std::string>();
                        if (!args_frag.empty()) {
                            StreamEventData delta_ev;
                            delta_ev.type            = StreamEventType::ContentBlockDelta;
                            delta_ev.index           = abs_index;
                            delta_ev.delta_type      = "input_json_delta";
                            // Accumulate raw argument fragments as a JSON string value
                            delta_ev.tool_input_delta = nlohmann::json(args_frag);
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
        StreamEventData md;
        md.type        = StreamEventType::MessageDelta;
        md.stop_reason = stop_reason_from_finish(finish_reason);
        md.usage       = usage;
        cb(md);

        StreamEventData ms;
        ms.type = StreamEventType::MessageStop;
        cb(ms);

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

    // Build httplib client
    httplib::Client client(scheme_host);
    client.set_connection_timeout(config_.connect_timeout_ms / 1000,
                                  (config_.connect_timeout_ms % 1000) * 1000);
    client.set_read_timeout(config_.read_timeout_ms / 1000,
                            (config_.read_timeout_ms % 1000) * 1000);
    client.set_write_timeout(30, 0);

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

    // SSE line buffer shared between the content receiver and the outer scope
    std::string  line_buf;
    bool         thinking_started = false;
    bool         thinking_open = false;
    bool         text_started = false;
    Usage        usage;
    bool         done = false;

    auto content_receiver = [&](const char* data, size_t len) -> bool {
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
                if (!parse_sse_line(payload, stream_cb, thinking_started, thinking_open, text_started, usage)) {
                    done = true;
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

    auto result = client.Post(post_path, headers, body.dump(), "application/json",
                              content_receiver);

    if (!result) {
        throw make_transport_error(result, post_path);
    }

    if (result->status < 200 || result->status >= 300) {
        throw make_http_status_error(*result, post_path);
    }

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

    // If MessageDelta was not sent yet (e.g., no finish_reason in stream), emit now
    if (!done) {
        StreamEventData md;
        md.type        = StreamEventType::MessageDelta;
        md.stop_reason = "end_turn";
        md.usage       = usage;
        stream_cb(md);

        StreamEventData ms;
        ms.type = StreamEventType::MessageStop;
        stream_cb(ms);
    }

    return usage;
}

}  // namespace omni::engine
