#include <gtest/gtest.h>
#include <tools/tool_registry.h>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// FakeTool — minimal concrete Tool for testing
// ---------------------------------------------------------------------------
class FakeTool : public Tool {
public:
    explicit FakeTool(std::string name, std::string description = "A fake tool")
        : name_(std::move(name)), description_(std::move(description)) {}

    std::string    name()        const override { return name_; }
    std::string    description() const override { return description_; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    bool is_read_only()   const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json& /*args*/) override {
        return {"ok", false};
    }

private:
    std::string name_;
    std::string description_;
};

// ---------------------------------------------------------------------------
// RegisterAndLookup
// ---------------------------------------------------------------------------
TEST(ToolRegistry, RegisterAndLookup) {
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>("read_file"));

    Tool* t = reg.get("read_file");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->name(), "read_file");
    EXPECT_EQ(reg.size(), 1u);
}

// ---------------------------------------------------------------------------
// LookupMissing
// ---------------------------------------------------------------------------
TEST(ToolRegistry, LookupMissing) {
    ToolRegistry reg;
    EXPECT_EQ(reg.get("nonexistent"), nullptr);
}

// ---------------------------------------------------------------------------
// ToolDefinitions — verify sorted by name and OpenAI format
// ---------------------------------------------------------------------------
TEST(ToolRegistry, ToolDefinitions) {
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>("zebra_tool",  "Does zebra things"));
    reg.register_tool(std::make_unique<FakeTool>("alpha_tool",  "Does alpha things"));
    reg.register_tool(std::make_unique<FakeTool>("middle_tool", "Does middle things"));

    const auto defs = reg.tool_definitions();
    ASSERT_EQ(defs.size(), 3u);

    // Must be sorted lexicographically
    EXPECT_EQ(defs[0]["function"]["name"], "alpha_tool");
    EXPECT_EQ(defs[1]["function"]["name"], "middle_tool");
    EXPECT_EQ(defs[2]["function"]["name"], "zebra_tool");

    // Verify OpenAI format
    for (const auto& d : defs) {
        EXPECT_EQ(d["type"], "function");
        EXPECT_TRUE(d["function"].contains("name"));
        EXPECT_TRUE(d["function"].contains("description"));
        EXPECT_TRUE(d["function"].contains("parameters"));
    }

    EXPECT_EQ(defs[0]["function"]["description"], "Does alpha things");
}

// ---------------------------------------------------------------------------
// RegisterOverwrites
// ---------------------------------------------------------------------------
TEST(ToolRegistry, RegisterOverwrites) {
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>("my_tool", "Version 1"));
    reg.register_tool(std::make_unique<FakeTool>("my_tool", "Version 2"));

    EXPECT_EQ(reg.size(), 1u);
    Tool* t = reg.get("my_tool");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->description(), "Version 2");
}

// ---------------------------------------------------------------------------
// Names — verify sorted
// ---------------------------------------------------------------------------
TEST(ToolRegistry, Names) {
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>("tool_c"));
    reg.register_tool(std::make_unique<FakeTool>("tool_a"));
    reg.register_tool(std::make_unique<FakeTool>("tool_b"));

    const auto ns = reg.names();
    ASSERT_EQ(ns.size(), 3u);
    EXPECT_EQ(ns[0], "tool_a");
    EXPECT_EQ(ns[1], "tool_b");
    EXPECT_EQ(ns[2], "tool_c");
}
