#include "web_tools.h"

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace omni::engine {

namespace {

std::string url_encode(const std::string& text) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<char>(ch);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(ch);
        }
    }
    return encoded.str();
}

void split_url(const std::string& url,
               std::string& scheme_host,
               std::string& path_prefix)
{
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos) {
        scheme_host = url;
        path_prefix = "";
        return;
    }

    const std::string scheme = url.substr(0, sep + 3);
    const std::string rest = url.substr(sep + 3);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos) {
        scheme_host = scheme + rest;
        path_prefix = "/";
    } else {
        scheme_host = scheme + rest.substr(0, slash);
        path_prefix = rest.substr(slash);
    }
}

ToolCallResult transport_error(const std::string& message) {
    return {message, true};
}

std::string collapse_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_space = true;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!last_space && !out.empty()) {
                out.push_back(' ');
            }
            last_space = true;
            continue;
        }
        out.push_back(static_cast<char>(ch));
        last_space = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string strip_html(const std::string& html) {
    std::string cleaned;
    cleaned.reserve(html.size());

    std::size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] == '<') {
            bool stripped_block = false;
            for (const char* tag : {"script", "style", "noscript", "svg"}) {
                const std::size_t tag_len = std::strlen(tag);
                if (pos + 1 + tag_len >= html.size()) {
                    continue;
                }

                bool match = true;
                for (std::size_t index = 0; index < tag_len; ++index) {
                    if (std::tolower(static_cast<unsigned char>(html[pos + 1 + index]))
                        != tag[index]) {
                        match = false;
                        break;
                    }
                }
                if (!match) {
                    continue;
                }

                const char after = html[pos + 1 + tag_len];
                if (after != '>' && !std::isspace(static_cast<unsigned char>(after))) {
                    continue;
                }

                const std::string close_tag = std::string{"</"} + tag;
                std::size_t end_pos = html.size();
                for (std::size_t search = pos + 1; search < html.size(); ++search) {
                    bool close_match = true;
                    if (search + close_tag.size() > html.size()) {
                        break;
                    }
                    for (std::size_t index = 0; index < close_tag.size(); ++index) {
                        if (std::tolower(static_cast<unsigned char>(html[search + index]))
                            != static_cast<unsigned char>(close_tag[index])) {
                            close_match = false;
                            break;
                        }
                    }
                    if (!close_match) {
                        continue;
                    }

                    const auto gt = html.find('>', search + close_tag.size());
                    end_pos = gt == std::string::npos ? html.size() : gt + 1;
                    break;
                }
                pos = end_pos;
                stripped_block = true;
                break;
            }
            if (stripped_block) {
                continue;
            }
        }

        cleaned.push_back(html[pos]);
        ++pos;
    }

    std::string result;
    result.reserve(cleaned.size());
    bool in_tag = false;
    for (char ch : cleaned) {
        if (ch == '<') {
            in_tag = true;
            result.push_back(' ');
            continue;
        }
        if (ch == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            result.push_back(ch);
        }
    }

    return collapse_whitespace(result);
}

ToolCallResult format_http_error(const httplib::Result& result,
                                 const std::string& url) {
    if (!result) {
        return transport_error("HTTP transport failure for '" + url + "': "
            + httplib::to_string(result.error()));
    }
    if (result->status >= 200 && result->status < 300) {
        return {"", false};
    }

    std::string message = "HTTP request to '" + url + "' failed with status "
        + std::to_string(result->status);
    if (!result->reason.empty()) {
        message += " (" + result->reason + ")";
    }
    if (!result->body.empty()) {
        message += ": " + result->body;
    }
    return transport_error(message);
}

std::string truncate_bytes(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    text.resize(max_bytes);
    text += "\n[truncated]";
    return text;
}

const char* brave_search_key() {
    return std::getenv("BRAVE_SEARCH_KEY");
}

}  // namespace

std::string WebFetchTool::name() const {
    return "web_fetch";
}

std::string WebFetchTool::description() const {
    return "Fetch a URL and return readable text content. HTML is converted into plain text and the result is truncated to a manageable size.";
}

nlohmann::json WebFetchTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"url", {{"type", "string"}, {"description", "Full HTTP or HTTPS URL to fetch."}}}
        }},
        {"required", {"url"}}
    };
}

ToolCallResult WebFetchTool::call(const nlohmann::json& args,
                                  const ToolContext&) {
    if (!args.contains("url") || !args["url"].is_string()) {
        return {"'url' must be a string", true};
    }

    const std::string url = args["url"].get<std::string>();
    std::string scheme_host;
    std::string path;
    split_url(url, scheme_host, path);
    if (scheme_host.empty() || path.empty()) {
        return {"invalid url: " + url, true};
    }

    httplib::Client client(scheme_host);
    client.set_follow_location(true);
    client.set_read_timeout(30, 0);
    client.set_write_timeout(30, 0);
    client.set_connection_timeout(10, 0);

    const auto result = client.Get(path);
    const auto error = format_http_error(result, url);
    if (error.is_error) {
        return error;
    }

    const auto content_type = result->get_header_value("Content-Type");
    std::string content = result->body;
    if (content_type.find("html") != std::string::npos) {
        content = strip_html(content);
    }
    return {truncate_bytes(content, kMaxBytes), false};
}

ToolCallResult WebFetchTool::call(const nlohmann::json&) {
    return {"web_fetch requires a tool context.", true};
}

std::string WebSearchTool::name() const {
    return "web_search";
}

std::string WebSearchTool::description() const {
    return "Search the public web via Brave Search and return concise result summaries. Requires BRAVE_SEARCH_KEY in the process environment.";
}

nlohmann::json WebSearchTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"query", {{"type", "string"}, {"description", "Search query."}}},
            {"count", {{"type", "integer"}, {"description", "Number of results to return."}, {"default", kDefaultCount}}}
        }},
        {"required", {"query"}}
    };
}

ToolCallResult WebSearchTool::call(const nlohmann::json& args,
                                   const ToolContext&) {
    if (!args.contains("query") || !args["query"].is_string()) {
        return {"'query' must be a string", true};
    }

    const char* key = brave_search_key();
    if (key == nullptr || std::string(key).empty()) {
        return {"BRAVE_SEARCH_KEY is not set. Set it in the environment seen by omni-engine-cli before using web_search.", true};
    }

    int count = args.value("count", kDefaultCount);
    count = std::clamp(count, 1, kMaxCount);
    const std::string query = args["query"].get<std::string>();
    const std::string url = "/res/v1/web/search?q=" + url_encode(query)
        + "&count=" + std::to_string(count);

    httplib::Headers headers{{"X-Subscription-Token", key}, {"Accept", "application/json"}};
    httplib::Client client("https://api.search.brave.com");
    client.set_follow_location(true);
    client.set_read_timeout(30, 0);
    client.set_write_timeout(30, 0);
    client.set_connection_timeout(10, 0);

    const auto result = client.Get(url, headers);
    const auto error = format_http_error(result, "https://api.search.brave.com" + url);
    if (error.is_error) {
        return error;
    }

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(result->body);
    } catch (const std::exception& ex) {
        return {std::string{"failed to parse Brave Search response: "} + ex.what(), true};
    }

    if (!payload.contains("web") || !payload["web"].contains("results")
        || !payload["web"]["results"].is_array()) {
        return {"Brave Search response did not contain web results.", true};
    }

    std::ostringstream out;
    int index = 1;
    for (const auto& entry : payload["web"]["results"]) {
        const std::string title = entry.value("title", std::string{"(untitled)"});
        const std::string link = entry.value("url", std::string{});
        const std::string description = entry.value("description", std::string{});
        out << index++ << ". " << title;
        if (!link.empty()) {
            out << "\n   " << link;
        }
        if (!description.empty()) {
            out << "\n   " << collapse_whitespace(description);
        }
        out << "\n";
        if (index > count) {
            break;
        }
    }

    std::string text = out.str();
    if (text.empty()) {
        text = "No results.";
    }
    return {truncate_bytes(text, WebFetchTool::kMaxBytes), false};
}

ToolCallResult WebSearchTool::call(const nlohmann::json&) {
    return {"web_search requires a tool context.", true};
}

}  // namespace omni::engine