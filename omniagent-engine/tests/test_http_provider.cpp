#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define private public
#include <omni/provider.h>
#include "core/stream_assembler.h"
#include "providers/http_provider.h"
#undef private

using namespace omni::engine;
using namespace std::chrono_literals;

TEST(HttpProvider, SendsToolChoiceAndAssemblesStreamedToolCalls) {
    HttpProviderConfig config;
    config.base_url = "http://127.0.0.1:11434/v1";
    config.model = "test-model";

    HttpProvider provider(config);

    CompletionRequest request;
    request.system_prompt = "Use tools.";
    request.tool_choice = "required";
    request.tools = {
        {
            {"type", "function"},
            {"function", {
                {"name", "read_file"},
                {"description", "Read a file"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}}},
                        {"start", {{"type", "integer"}}}
                    }},
                    {"required", nlohmann::json::array({"path"})}
                }}
            }}
        }
    };

    Message user_message;
    user_message.role = Role::User;
    user_message.content = {ContentBlock{TextContent{"Inspect the workspace."}}};
    request.messages = {user_message};

    const nlohmann::json body = provider.build_request_body(request);
    EXPECT_EQ(body["tool_choice"], "required");
    ASSERT_TRUE(body.contains("tools"));

    StreamAssembler assembler;
    StreamEventData message_start;
    message_start.type = StreamEventType::MessageStart;
    assembler.process(message_start);

    bool thinking_started = false;
    bool thinking_open = false;
    bool text_started = false;
    std::unordered_set<int> open_tool_blocks;
    Usage usage;
    bool message_terminated = false;

    const std::string chunk_one =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"README"}}]}}]})";
    const std::string chunk_two =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":".md\",\"start\":1}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":11,"completion_tokens":7}})";

    StreamCallback stream_cb = [&](const StreamEventData& event) {
        assembler.process(event);
    };

    EXPECT_TRUE(provider.parse_sse_line(chunk_one, stream_cb, thinking_started, thinking_open,
                                        text_started, open_tool_blocks, usage,
                                        message_terminated));
    EXPECT_FALSE(provider.parse_sse_line(chunk_two, stream_cb, thinking_started, thinking_open,
                                         text_started, open_tool_blocks, usage,
                                         message_terminated));

    EXPECT_EQ(usage.input_tokens, 11);
    EXPECT_EQ(usage.output_tokens, 7);
    EXPECT_TRUE(message_terminated);
    EXPECT_EQ(assembler.stop_reason(), "tool_use");

    auto blocks = assembler.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 1u);
    const auto* tool = std::get_if<ToolUseContent>(&blocks[0].data);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->id, "call_1");
    EXPECT_EQ(tool->name, "read_file");
    ASSERT_TRUE(tool->input.is_object());
    EXPECT_EQ(tool->input["path"], "README.md");
    EXPECT_EQ(tool->input["start"], 1);
}

TEST(HttpProvider, IncludesSamplingFieldsAndGuardsLocalOnlyOnRemote) {
    CompletionRequest request;
    request.temperature = 0.6;
    request.top_p = 0.9;
    request.top_k = 32;
    request.min_p = 0.07;
    request.presence_penalty = 0.4;
    request.frequency_penalty = -0.2;
    request.max_tokens = 2048;

    HttpProviderConfig local_config;
    local_config.base_url = "http://127.0.0.1:11434/v1";
    local_config.model = "local-model";
    HttpProvider local_provider(local_config);

    const nlohmann::json local_body = local_provider.build_request_body(request);
    EXPECT_EQ(local_body["temperature"], 0.6);
    EXPECT_EQ(local_body["top_p"], 0.9);
    EXPECT_EQ(local_body["top_k"], 32);
    EXPECT_EQ(local_body["min_p"], 0.07);
    EXPECT_EQ(local_body["presence_penalty"], 0.4);
    EXPECT_EQ(local_body["frequency_penalty"], -0.2);
    EXPECT_EQ(local_body["max_tokens"], 2048);

    HttpProviderConfig remote_config;
    remote_config.base_url = "https://api.openai.com/v1";
    remote_config.model = "remote-model";
    HttpProvider remote_provider(remote_config);

    const nlohmann::json remote_body = remote_provider.build_request_body(request);
    EXPECT_EQ(remote_body["temperature"], 0.6);
    EXPECT_EQ(remote_body["top_p"], 0.9);
    EXPECT_FALSE(remote_body.contains("top_k"));
    EXPECT_FALSE(remote_body.contains("min_p"));
    EXPECT_EQ(remote_body["presence_penalty"], 0.4);
    EXPECT_EQ(remote_body["frequency_penalty"], -0.2);
    EXPECT_EQ(remote_body["max_tokens"], 2048);
}

TEST(HttpProvider, StopFlagAbortsBlockedRequest) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX socket regression test";
#else
    std::mutex mutex;
    std::condition_variable request_received_cv;
    std::condition_variable release_response_cv;
    bool request_received = false;
    bool release_response = false;

    auto recv_all = [](int fd, std::string& buffer) -> ssize_t {
        char chunk[4096];
        const ssize_t bytes_read = ::recv(fd, chunk, sizeof(chunk), 0);
        if (bytes_read > 0) {
            buffer.append(chunk, static_cast<std::size_t>(bytes_read));
        }
        return bytes_read;
    };

    auto read_http_request = [&](int fd) {
        std::string request;
        std::size_t content_length = 0;
        bool header_complete = false;

        while (true) {
            const ssize_t bytes_read = recv_all(fd, request);
            if (bytes_read <= 0) {
                return;
            }

            if (!header_complete) {
                const std::size_t header_end = request.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    continue;
                }

                header_complete = true;
                const std::string_view headers(request.data(), header_end + 4);
                const std::size_t length_pos = headers.find("Content-Length:");
                if (length_pos != std::string_view::npos) {
                    const std::size_t value_start = headers.find_first_not_of(' ', length_pos + 15);
                    const std::size_t value_end = headers.find("\r\n", value_start);
                    content_length = static_cast<std::size_t>(std::stoul(
                        std::string(headers.substr(value_start, value_end - value_start))));
                }

                const std::size_t body_bytes = request.size() - (header_end + 4);
                if (body_bytes >= content_length) {
                    return;
                }
                continue;
            }

            const std::size_t header_end = request.find("\r\n\r\n");
            const std::size_t body_bytes = request.size() - (header_end + 4);
            if (body_bytes >= content_length) {
                return;
            }
        }
    };

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0) << std::strerror(errno);

    const int reuse = 1;
    ASSERT_EQ(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), 0)
        << std::strerror(errno);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
    ASSERT_EQ(::bind(listener,
                     reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)),
              0) << std::strerror(errno);
    ASSERT_EQ(::listen(listener, 1), 0) << std::strerror(errno);

    socklen_t address_len = sizeof(address);
    ASSERT_EQ(::getsockname(listener,
                           reinterpret_cast<sockaddr*>(&address),
                           &address_len),
              0) << std::strerror(errno);
    const int port = ntohs(address.sin_port);

    std::thread server_thread([&]() {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            return;
        }

        read_http_request(client);
        {
            std::lock_guard<std::mutex> lock(mutex);
            request_received = true;
        }
        request_received_cv.notify_all();

        std::unique_lock<std::mutex> lock(mutex);
        release_response_cv.wait(lock, [&]() { return release_response; });
        lock.unlock();

        ::shutdown(client, SHUT_RDWR);
        ::close(client);
        ::close(listener);
    });

    HttpProviderConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    config.model = "test-model";
    config.read_timeout_ms = 30000;

    HttpProvider provider(config);

    CompletionRequest request;
    Message user_message;
    user_message.role = Role::User;
    user_message.content = {ContentBlock{TextContent{"interrupt me"}}};
    request.messages = {user_message};

    std::atomic<bool> stop_flag{false};
    auto future = std::async(std::launch::async, [&]() {
        return provider.complete(request, [](const StreamEventData&) {}, stop_flag);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(request_received_cv.wait_for(lock, 1s, [&]() { return request_received; }));
    }

    stop_flag.store(true, std::memory_order_relaxed);
    const auto status = future.wait_for(1s);

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_response = true;
    }
    release_response_cv.notify_all();

    if (status != std::future_status::ready) {
        server_thread.join();
        FAIL() << "provider did not abort promptly after stop request";
    }

    EXPECT_NO_THROW((void)future.get());

    server_thread.join();
#endif
}