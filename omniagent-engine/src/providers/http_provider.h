#pragma once

#include <omni/provider.h>
#include <string>
#include <unordered_set>

namespace omni::engine {

struct HttpProviderConfig {
    std::string base_url;             // e.g., "http://localhost:11434/v1"
    std::string api_key;              // empty for local models
    std::string model;                // e.g., "qwen3.5:32b"
    int         max_context_tokens = 32768;
    int         max_output_tokens  = 8192;
    int         connect_timeout_ms = 10000;
    int         read_timeout_ms    = 120000;
    bool        supports_thinking  = false;
    bool        supports_images    = false;
};

class HttpProvider : public LLMProvider {
public:
    explicit HttpProvider(HttpProviderConfig config);

    Usage complete(const CompletionRequest& request, StreamCallback stream_cb,
                   std::atomic<bool>& stop_flag) override;
    ModelCapabilities capabilities() const override;
    std::string name() const override;

private:
    HttpProviderConfig config_;

    nlohmann::json build_request_body(const CompletionRequest& request) const;
    nlohmann::json message_to_openai(const Message& msg) const;
    bool parse_sse_line(const std::string& data, StreamCallback& cb,
                        bool& thinking_started,
                        bool& thinking_open,
                        bool& text_started,
                        std::unordered_set<int>& open_tool_blocks,
                        Usage& usage) const;
};

}  // namespace omni::engine
