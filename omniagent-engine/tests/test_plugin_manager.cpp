#include <gtest/gtest.h>

#include <omni/plugin.h>
#include <omni/tool.h>
#include "plugins/plugin_manager.h"
#include "tools/tool_registry.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

/// A minimal concrete Tool for plugin tests.
class PM_FakeTool : public Tool {
public:
    explicit PM_FakeTool(std::string name) : name_(std::move(name)) {}

    std::string    name()         const override { return name_; }
    std::string    description()  const override { return "plugin tool"; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"ok", false}; }

private:
    std::string name_;
};

/// A test Plugin that provides a fixed set of tools.
class PM_TestPlugin : public Plugin {
public:
    PM_TestPlugin(std::string name,
                  std::vector<std::string> tool_names,
                  std::string version     = "1.0",
                  std::string description = "test plugin")
        : name_(std::move(name))
        , tool_names_(std::move(tool_names))
        , version_(std::move(version))
        , description_(std::move(description))
    {}

    PluginManifest manifest() const override {
        PluginManifest m;
        m.name        = name_;
        m.version     = version_;
        m.description = description_;
        m.tool_names  = tool_names_;
        return m;
    }

    std::vector<std::unique_ptr<Tool>> create_tools() override {
        std::vector<std::unique_ptr<Tool>> tools;
        for (const auto& tn : tool_names_) {
            tools.push_back(std::make_unique<PM_FakeTool>(tn));
        }
        return tools;
    }

private:
    std::string              name_;
    std::vector<std::string> tool_names_;
    std::string              version_;
    std::string              description_;
};

// ---------------------------------------------------------------------------
// PluginManifest struct construction
// ---------------------------------------------------------------------------

TEST(PluginManifest, Construction) {
    PluginManifest m;
    m.name        = "my_plugin";
    m.version     = "2.1.0";
    m.description = "does stuff";
    m.tool_names  = {"tool_a", "tool_b"};
    m.command_names = {"/cmd"};

    EXPECT_EQ(m.name,        "my_plugin");
    EXPECT_EQ(m.version,     "2.1.0");
    EXPECT_EQ(m.description, "does stuff");
    ASSERT_EQ(m.tool_names.size(),    2u);
    EXPECT_EQ(m.tool_names[0],        "tool_a");
    EXPECT_EQ(m.tool_names[1],        "tool_b");
    ASSERT_EQ(m.command_names.size(), 1u);
    EXPECT_EQ(m.command_names[0],     "/cmd");
}

// ---------------------------------------------------------------------------
// ListEmpty: fresh manager returns empty list
// ---------------------------------------------------------------------------

TEST(PluginManager, ListEmpty) {
    ToolRegistry   reg;
    PluginManager  mgr(reg);

    EXPECT_EQ(mgr.count(), 0u);
    EXPECT_TRUE(mgr.list().empty());
}

// ---------------------------------------------------------------------------
// LoadNonExistent: load of a non-existent path returns false
// ---------------------------------------------------------------------------

TEST(PluginManager, LoadNonExistent) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    const bool ok = mgr.load("/tmp/omni_no_such_plugin_xyz.so");
    EXPECT_FALSE(ok);
    EXPECT_EQ(mgr.count(), 0u);
}

// ---------------------------------------------------------------------------
// LoadDirectoryEmpty: loading an empty (or non-existent) directory returns 0
// ---------------------------------------------------------------------------

TEST(PluginManager, LoadDirectoryEmpty) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    // Non-existent directory
    EXPECT_EQ(mgr.load_directory("/tmp/omni_no_such_plugin_dir_xyz"), 0);

    // Existing but empty temporary directory
    const auto tmp = std::filesystem::temp_directory_path() / "omni_plugin_test_empty";
    std::filesystem::create_directories(tmp);
    EXPECT_EQ(mgr.load_directory(tmp), 0);
    std::filesystem::remove(tmp);
}

// ---------------------------------------------------------------------------
// RegisterPlugin: tools appear in registry after registration
// ---------------------------------------------------------------------------

TEST(PluginManager, RegisterPlugin) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    auto plugin = std::make_unique<PM_TestPlugin>(
        "test_plugin", std::vector<std::string>{"pm_tool_a", "pm_tool_b"});

    const bool ok = mgr.register_plugin(std::move(plugin));
    EXPECT_TRUE(ok);
    EXPECT_EQ(mgr.count(), 1u);

    // Tools must now be in the registry.
    EXPECT_NE(reg.get("pm_tool_a"), nullptr);
    EXPECT_NE(reg.get("pm_tool_b"), nullptr);
    EXPECT_EQ(reg.size(), 2u);
}

// ---------------------------------------------------------------------------
// RegisterPlugin: duplicate name is rejected
// ---------------------------------------------------------------------------

TEST(PluginManager, RegisterPluginDuplicateName) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    auto p1 = std::make_unique<PM_TestPlugin>("dup_plugin", std::vector<std::string>{"tool_x"});
    auto p2 = std::make_unique<PM_TestPlugin>("dup_plugin", std::vector<std::string>{"tool_y"});

    EXPECT_TRUE(mgr.register_plugin(std::move(p1)));
    EXPECT_FALSE(mgr.register_plugin(std::move(p2)));  // same name → rejected
    EXPECT_EQ(mgr.count(), 1u);
    // Only tool_x should be registered.
    EXPECT_NE(reg.get("tool_x"), nullptr);
    EXPECT_EQ(reg.get("tool_y"), nullptr);
}

// ---------------------------------------------------------------------------
// UnloadPlugin: tools removed from registry after unload
// ---------------------------------------------------------------------------

TEST(PluginManager, UnloadPlugin) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    auto plugin = std::make_unique<PM_TestPlugin>(
        "removable_plugin", std::vector<std::string>{"pm_tool_c", "pm_tool_d"});

    ASSERT_TRUE(mgr.register_plugin(std::move(plugin)));
    EXPECT_EQ(mgr.count(), 1u);

    // Tools present before unload.
    EXPECT_NE(reg.get("pm_tool_c"), nullptr);
    EXPECT_NE(reg.get("pm_tool_d"), nullptr);

    const bool unloaded = mgr.unload("removable_plugin");
    EXPECT_TRUE(unloaded);
    EXPECT_EQ(mgr.count(), 0u);

    // Tools must be gone from the registry.
    EXPECT_EQ(reg.get("pm_tool_c"), nullptr);
    EXPECT_EQ(reg.get("pm_tool_d"), nullptr);
    EXPECT_EQ(reg.size(), 0u);
}

// ---------------------------------------------------------------------------
// UnloadNonExistent: unloading an unknown name returns false
// ---------------------------------------------------------------------------

TEST(PluginManager, UnloadNonExistent) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    EXPECT_FALSE(mgr.unload("ghost_plugin"));
}

// ---------------------------------------------------------------------------
// ListPlugins: two registered plugins appear in the list
// ---------------------------------------------------------------------------

TEST(PluginManager, ListPlugins) {
    ToolRegistry  reg;
    PluginManager mgr(reg);

    auto p1 = std::make_unique<PM_TestPlugin>(
        "plugin_alpha", std::vector<std::string>{"alpha_tool"}, "1.0", "Alpha plugin");
    auto p2 = std::make_unique<PM_TestPlugin>(
        "plugin_beta",  std::vector<std::string>{"beta_tool"},  "2.3", "Beta plugin");

    ASSERT_TRUE(mgr.register_plugin(std::move(p1)));
    ASSERT_TRUE(mgr.register_plugin(std::move(p2)));
    EXPECT_EQ(mgr.count(), 2u);

    const auto manifests = mgr.list();
    ASSERT_EQ(manifests.size(), 2u);

    // Find each by name (order not guaranteed from unordered_map).
    bool found_alpha = false, found_beta = false;
    for (const auto& m : manifests) {
        if (m.name == "plugin_alpha") {
            found_alpha = true;
            EXPECT_EQ(m.version,     "1.0");
            EXPECT_EQ(m.description, "Alpha plugin");
            ASSERT_EQ(m.tool_names.size(), 1u);
            EXPECT_EQ(m.tool_names[0], "alpha_tool");
        } else if (m.name == "plugin_beta") {
            found_beta = true;
            EXPECT_EQ(m.version,     "2.3");
            EXPECT_EQ(m.description, "Beta plugin");
            ASSERT_EQ(m.tool_names.size(), 1u);
            EXPECT_EQ(m.tool_names[0], "beta_tool");
        }
    }
    EXPECT_TRUE(found_alpha);
    EXPECT_TRUE(found_beta);
}

// ---------------------------------------------------------------------------
// DestructorUnregistersTools: manager destructor cleans up registry entries
// ---------------------------------------------------------------------------

TEST(PluginManager, DestructorUnregistersTools) {
    ToolRegistry reg;

    {
        PluginManager mgr(reg);
        auto plugin = std::make_unique<PM_TestPlugin>(
            "scoped_plugin", std::vector<std::string>{"scoped_tool"});
        ASSERT_TRUE(mgr.register_plugin(std::move(plugin)));
        EXPECT_NE(reg.get("scoped_tool"), nullptr);
    }  // mgr destroyed here

    // Tool should be gone from the registry.
    EXPECT_EQ(reg.get("scoped_tool"), nullptr);
}
