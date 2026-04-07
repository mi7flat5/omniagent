#include <gtest/gtest.h>

#include "tools/workspace_tools.h"

#include <filesystem>
#include <fstream>

using namespace omni::engine;

namespace {

namespace fs = std::filesystem;

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open()) << path;
    stream << content;
    ASSERT_TRUE(stream.good()) << path;
}

class WorkspaceToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        workspace_root_ = fs::temp_directory_path() / fs::path("omni_engine_workspace_tools");
        fs::remove_all(workspace_root_);
        fs::create_directories(workspace_root_ / "src");
        fs::create_directories(workspace_root_ / "nested");
        write_text(workspace_root_ / "README.md", "hello\nproject\n");
        write_text(workspace_root_ / "src" / "main.cpp", "int main() { return 0; }\n");
        write_text(workspace_root_ / "nested" / "notes.txt", "needle here\n");

        context_.project_id = "workspace-tools";
        context_.session_id = "session";
        context_.run_id = "run";
        context_.profile = "bugfix";
        context_.workspace_root = workspace_root_;
        context_.working_dir = workspace_root_;
    }

    void TearDown() override {
        fs::remove_all(workspace_root_);
    }

    fs::path workspace_root_;
    ToolContext context_;
};

}  // namespace

TEST_F(WorkspaceToolsTest, ReadListGlobAndGrepInspectWorkspace) {
    ReadFileTool read_file;
    ListDirTool list_dir;
    GlobTool glob;
    GrepTool grep;

    const auto read_result = read_file.call(
        nlohmann::json{{"path", "README.md"}, {"start_line", 1}, {"end_line", 2}},
        context_);
    EXPECT_FALSE(read_result.is_error);
    EXPECT_NE(read_result.content.find("README.md"), std::string::npos);
    EXPECT_NE(read_result.content.find("1: hello"), std::string::npos);

    const auto list_result = list_dir.call(nlohmann::json{{"path", "."}}, context_);
    EXPECT_FALSE(list_result.is_error);
    EXPECT_NE(list_result.content.find("README.md"), std::string::npos);
    EXPECT_NE(list_result.content.find("src/"), std::string::npos);

    const auto glob_result = glob.call(nlohmann::json{{"pattern", "src/**/*.cpp"}}, context_);
    EXPECT_FALSE(glob_result.is_error);
    EXPECT_NE(glob_result.content.find("src/main.cpp"), std::string::npos);

    const auto grep_result = grep.call(nlohmann::json{{"pattern", "needle"}}, context_);
    EXPECT_FALSE(grep_result.is_error);
    EXPECT_NE(grep_result.content.find("nested/notes.txt:1:needle here"), std::string::npos);
}

TEST_F(WorkspaceToolsTest, WriteEditAndDeleteModifyWorkspace) {
    WriteFileTool write_file;
    EditFileTool edit_file;
    DeleteFileTool delete_file;

    const auto write_result = write_file.call(
        nlohmann::json{{"path", "src/generated.txt"}, {"content", "before value\n"}},
        context_);
    EXPECT_FALSE(write_result.is_error);
    EXPECT_TRUE(fs::exists(workspace_root_ / "src" / "generated.txt"));

    const auto edit_result = edit_file.call(
        nlohmann::json{{"path", "src/generated.txt"},
                       {"old_string", "before"},
                       {"new_string", "after"}},
        context_);
    EXPECT_FALSE(edit_result.is_error);

    ReadFileTool read_file;
    const auto read_result = read_file.call(
        nlohmann::json{{"path", "src/generated.txt"}},
        context_);
    EXPECT_FALSE(read_result.is_error);
    EXPECT_NE(read_result.content.find("after value"), std::string::npos);

    const auto delete_result = delete_file.call(
        nlohmann::json{{"path", "src/generated.txt"}},
        context_);
    EXPECT_FALSE(delete_result.is_error);
    EXPECT_FALSE(fs::exists(workspace_root_ / "src" / "generated.txt"));
}

TEST_F(WorkspaceToolsTest, RejectsWorkspaceEscape) {
    ReadFileTool read_file;

    const auto result = read_file.call(
        nlohmann::json{{"path", "../outside.txt"}},
        context_);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("escapes workspace root"), std::string::npos);
}

#ifndef _WIN32
TEST_F(WorkspaceToolsTest, BashRunsInsideRequestedWorkingDirectory) {
    BashTool bash;

    const auto result = bash.call(
        nlohmann::json{{"command", "pwd && printf marker"},
                       {"working_dir", "nested"},
                       {"timeout", 5000}},
        context_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find((workspace_root_ / "nested").string()), std::string::npos);
    EXPECT_NE(result.content.find("marker"), std::string::npos);
}
#endif