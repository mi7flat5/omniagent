#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace omni::engine {

struct MemoryEntry {
    std::string name;
    std::string description;
    std::string type;     // "user", "feedback", "project", "reference"
    std::string content;  // full file content (after frontmatter)
};

class MemoryLoader {
public:
    /// Load memory entries from a directory.
    /// Reads MEMORY.md index, then loads referenced .md files.
    /// Also loads AGENT.md if found (project instructions).
    void load(const std::filesystem::path& project_dir);

    /// Get all loaded entries.
    const std::vector<MemoryEntry>& entries() const { return entries_; }

    /// Get the AGENT.md content (project instructions).
    const std::string& project_instructions() const { return project_instructions_; }

    /// Build a system prompt section from loaded memory.
    /// Returns text suitable for injection into the system prompt.
    std::string build_context() const;

private:
    std::vector<MemoryEntry> entries_;
    std::string              project_instructions_;

    /// Parse frontmatter (---\n...\n---) from a markdown file.
    static MemoryEntry parse_memory_file(const std::filesystem::path& path);
};

}  // namespace omni::engine
