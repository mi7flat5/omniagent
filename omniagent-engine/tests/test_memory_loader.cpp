#include <gtest/gtest.h>

#include "services/memory_loader.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace omni::engine;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: write a file to disk
// ---------------------------------------------------------------------------

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open()) << "Could not create: " << path;
    f << content;
}

// ---------------------------------------------------------------------------
// Fixture: creates and cleans up a temp directory
// ---------------------------------------------------------------------------

class MemoryLoaderTest : public ::testing::Test {
protected:
    fs::path tmp_dir;

    void SetUp() override {
        tmp_dir = fs::temp_directory_path() / ("omni_memtest_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }
};

// ---------------------------------------------------------------------------
// LoadFromDirectory — MEMORY.md index + referenced entries are loaded
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, LoadFromDirectory) {
    const fs::path mem_dir = tmp_dir / ".omniagent" / "memory";
    fs::create_directories(mem_dir);

    // Write an entry file with frontmatter
    write_file(mem_dir / "entry1.md",
               "---\n"
               "name: TestEntry\n"
               "description: A test memory entry\n"
               "type: project\n"
               "---\n"
               "This is the entry content.\n");

    // Write the index MEMORY.md referencing entry1.md
    write_file(mem_dir / "MEMORY.md",
               "- [TestEntry](entry1.md) — A test memory entry\n");

    MemoryLoader loader;
    loader.load(tmp_dir);

    ASSERT_EQ(loader.entries().size(), 1u);
    EXPECT_EQ(loader.entries()[0].name, "TestEntry");
    EXPECT_EQ(loader.entries()[0].type, "project");
    EXPECT_NE(loader.entries()[0].content.find("entry content"), std::string::npos);
}

// ---------------------------------------------------------------------------
// BuildContext — loaded entries appear in build_context() output
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, BuildContext) {
    const fs::path mem_dir = tmp_dir / ".omniagent" / "memory";
    fs::create_directories(mem_dir);

    write_file(mem_dir / "facts.md",
               "---\n"
               "name: ImportantFacts\n"
               "type: reference\n"
               "---\n"
               "The sky is blue.\n");

    write_file(mem_dir / "MEMORY.md",
               "- [ImportantFacts](facts.md) — Some facts\n");

    write_file(tmp_dir / "AGENT.md", "Be concise and direct.\n");

    MemoryLoader loader;
    loader.load(tmp_dir);

    const std::string ctx = loader.build_context();

    EXPECT_NE(ctx.find("Project Instructions"), std::string::npos);
    EXPECT_NE(ctx.find("Be concise and direct"), std::string::npos);
    EXPECT_NE(ctx.find("Memory Context"), std::string::npos);
    EXPECT_NE(ctx.find("ImportantFacts"), std::string::npos);
    EXPECT_NE(ctx.find("The sky is blue"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ParseFrontmatter — name/description/type correctly extracted
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, ParseFrontmatter) {
    const fs::path mem_dir = tmp_dir / ".omniagent" / "memory";
    fs::create_directories(mem_dir);

    write_file(mem_dir / "typed.md",
               "---\n"
               "name: MyEntry\n"
               "description: Explains things\n"
               "type: feedback\n"
               "---\n"
               "Content goes here.\n");

    write_file(mem_dir / "MEMORY.md", "- [MyEntry](typed.md)\n");

    MemoryLoader loader;
    loader.load(tmp_dir);

    ASSERT_EQ(loader.entries().size(), 1u);
    const auto& e = loader.entries()[0];
    EXPECT_EQ(e.name,        "MyEntry");
    EXPECT_EQ(e.description, "Explains things");
    EXPECT_EQ(e.type,        "feedback");
    EXPECT_NE(e.content.find("Content goes here"), std::string::npos);
}

// ---------------------------------------------------------------------------
// MissingDirectory — loading from nonexistent path produces empty entries
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, MissingDirectory) {
    MemoryLoader loader;
    EXPECT_NO_THROW(loader.load("/nonexistent/path/that/does/not/exist/xyz"));
    EXPECT_TRUE(loader.entries().empty());
    EXPECT_TRUE(loader.project_instructions().empty());
    EXPECT_TRUE(loader.build_context().empty());
}

// ---------------------------------------------------------------------------
// NoMemoryIndex — directory exists but has no MEMORY.md → no entries, no crash
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, NoMemoryIndex) {
    MemoryLoader loader;
    EXPECT_NO_THROW(loader.load(tmp_dir));
    EXPECT_TRUE(loader.entries().empty());
}

// ---------------------------------------------------------------------------
// AgentMdLoaded — AGENT.md in project root is loaded as project instructions
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, AgentMdLoaded) {
    write_file(tmp_dir / "AGENT.md", "# Project Rules\nAlways be helpful.\n");

    MemoryLoader loader;
    loader.load(tmp_dir);

    EXPECT_NE(loader.project_instructions().find("Always be helpful"), std::string::npos);
}

// ---------------------------------------------------------------------------
// MultipleEntries — multiple files in MEMORY.md index all loaded
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, MultipleEntries) {
    const fs::path mem_dir = tmp_dir / ".omniagent" / "memory";
    fs::create_directories(mem_dir);

    write_file(mem_dir / "a.md",
               "---\nname: Alpha\ntype: user\n---\nAlpha content.\n");
    write_file(mem_dir / "b.md",
               "---\nname: Beta\ntype: project\n---\nBeta content.\n");
    write_file(mem_dir / "c.md",
               "---\nname: Gamma\ntype: feedback\n---\nGamma content.\n");

    write_file(mem_dir / "MEMORY.md",
               "- [Alpha](a.md) — first\n"
               "- [Beta](b.md) — second\n"
               "- [Gamma](c.md) — third\n");

    MemoryLoader loader;
    loader.load(tmp_dir);

    ASSERT_EQ(loader.entries().size(), 3u);
    EXPECT_EQ(loader.entries()[0].name, "Alpha");
    EXPECT_EQ(loader.entries()[1].name, "Beta");
    EXPECT_EQ(loader.entries()[2].name, "Gamma");
}

// ---------------------------------------------------------------------------
// EmptyContext — no entries, no instructions → build_context returns empty
// ---------------------------------------------------------------------------

TEST_F(MemoryLoaderTest, EmptyContext) {
    MemoryLoader loader;
    loader.load(tmp_dir);
    EXPECT_TRUE(loader.build_context().empty());
}
