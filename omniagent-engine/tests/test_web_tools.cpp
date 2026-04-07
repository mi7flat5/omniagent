#include <gtest/gtest.h>

#include "tools/web_tools.h"
#include "tools/workspace_tools.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace omni::engine;

namespace {

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name) {
        if (const char* existing = std::getenv(name_)) {
            had_value_ = true;
            old_value_ = existing;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            set(old_value_);
        } else {
            unset();
        }
    }

    void unset() {
#ifdef _WIN32
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    const char* name_;
    bool had_value_ = false;
    std::string old_value_;
};

std::vector<std::string> collect_names(std::vector<std::unique_ptr<Tool>> tools) {
    std::vector<std::string> names;
    for (const auto& tool : tools) {
        names.push_back(tool->name());
    }
    return names;
}

}  // namespace

TEST(WebToolsTest, DefaultWorkspaceToolsIncludeWebTools) {
    const auto names = collect_names(make_default_workspace_tools());

    EXPECT_NE(std::find(names.begin(), names.end(), "web_fetch"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "web_search"), names.end());
}

TEST(WebToolsTest, WebSearchRequiresBraveSearchKey) {
    ScopedEnvVar brave_key("BRAVE_SEARCH_KEY");
    brave_key.unset();

    WebSearchTool tool;
    ToolContext context;
    const auto result = tool.call(nlohmann::json{{"query", "omniagent"}}, context);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("BRAVE_SEARCH_KEY"), std::string::npos);
}

TEST(WebToolsTest, WebFetchRejectsInvalidUrl) {
    WebFetchTool tool;
    ToolContext context;
    const auto result = tool.call(nlohmann::json{{"url", "not-a-url"}}, context);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("invalid url"), std::string::npos);
}