#include "memory_loader.h"

#include <fstream>
#include <regex>
#include <sstream>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Extract filename from a MEMORY.md link line:
//   - [Title](filename.md) — description
// Returns empty string if the line doesn't match.
static std::string extract_filename(const std::string& line) {
    // Match markdown link syntax: [text](filename)
    static const std::regex link_re(R"(\[([^\]]*)\]\(([^)]+)\))");
    std::smatch m;
    if (std::regex_search(line, m, link_re)) {
        return m[2].str();
    }
    return {};
}

// ---------------------------------------------------------------------------
// parse_memory_file
// ---------------------------------------------------------------------------

MemoryEntry MemoryLoader::parse_memory_file(const std::filesystem::path& path) {
    MemoryEntry entry;
    entry.name = path.stem().string();  // default: filename without extension

    const std::string raw = read_file(path);
    if (raw.empty()) return entry;

    // Try to find YAML-like frontmatter delimited by --- lines.
    // Frontmatter is optional; if absent the whole file is content.
    const std::string delimiter = "---";
    std::string content_start;

    std::istringstream stream(raw);
    std::string line;
    bool in_frontmatter = false;
    bool frontmatter_done = false;
    std::vector<std::string> frontmatter_lines;
    std::ostringstream content_buf;
    bool first_line = true;

    while (std::getline(stream, line)) {
        if (first_line) {
            first_line = false;
            if (line == delimiter || line == "---\r") {
                in_frontmatter = true;
                continue;
            }
            // No frontmatter — entire file is content.
            content_buf << line << '\n';
            continue;
        }

        if (in_frontmatter) {
            if (line == delimiter || line == "---\r") {
                in_frontmatter = false;
                frontmatter_done = true;
                continue;
            }
            frontmatter_lines.push_back(line);
        } else {
            content_buf << line << '\n';
        }
    }

    // If we never closed the frontmatter, treat everything as content.
    if (in_frontmatter && !frontmatter_done) {
        // Malformed — just dump frontmatter lines back as content.
        for (const auto& fl : frontmatter_lines) {
            content_buf << fl << '\n';
        }
        entry.content = content_buf.str();
        return entry;
    }

    entry.content = content_buf.str();

    // Parse simple key: value frontmatter lines.
    for (const auto& fl : frontmatter_lines) {
        const auto colon = fl.find(':');
        if (colon == std::string::npos) continue;
        std::string key   = fl.substr(0, colon);
        std::string value = fl.substr(colon + 1);

        // Trim whitespace from key and value.
        auto trim = [](std::string& s) {
            const auto not_space = [](unsigned char c){ return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        };
        trim(key);
        trim(value);

        if (key == "name")        entry.name        = value;
        else if (key == "description") entry.description = value;
        else if (key == "type")   entry.type        = value;
    }

    return entry;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

void MemoryLoader::load(const std::filesystem::path& project_dir) {
    entries_.clear();
    project_instructions_.clear();

    if (project_dir.empty() || !std::filesystem::exists(project_dir)) return;

    // Look for MEMORY.md index in the omniagent directory.
    const std::vector<std::filesystem::path> memory_candidates = {
        project_dir / ".omniagent" / "memory" / "MEMORY.md",
    };

    std::filesystem::path memory_dir;
    std::filesystem::path memory_index;

    for (const auto& candidate : memory_candidates) {
        if (std::filesystem::exists(candidate)) {
            memory_index = candidate;
            memory_dir   = candidate.parent_path();
            break;
        }
    }

    if (!memory_index.empty()) {
        const std::string index_content = read_file(memory_index);
        std::istringstream stream(index_content);
        std::string line;
        while (std::getline(stream, line)) {
            const std::string filename = extract_filename(line);
            if (filename.empty()) continue;

            const std::filesystem::path entry_path = memory_dir / filename;
            if (!std::filesystem::exists(entry_path)) continue;

            MemoryEntry entry = parse_memory_file(entry_path);

            // If no description was found in frontmatter, try to parse it from the
            // index line (text after the em-dash separator).
            if (entry.description.empty()) {
                const auto dash = line.find(" \xe2\x80\x94 ");  // UTF-8 em-dash
                if (dash != std::string::npos) {
                    entry.description = line.substr(dash + 5);  // skip " — " (5 bytes)
                    // Trim whitespace
                    auto trim_ws = [](std::string& s) {
                        const auto not_space = [](unsigned char c){ return !std::isspace(c); };
                        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
                        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
                    };
                    trim_ws(entry.description);
                }
            }

            entries_.push_back(std::move(entry));
        }
    }

    // Look for AGENT.md / project instructions in known locations.
    const std::vector<std::filesystem::path> agent_candidates = {
        project_dir / "AGENT.md",
        project_dir / ".omniagent" / "AGENT.md",
    };

    for (const auto& candidate : agent_candidates) {
        if (std::filesystem::exists(candidate)) {
            project_instructions_ = read_file(candidate);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// build_context
// ---------------------------------------------------------------------------

std::string MemoryLoader::build_context() const {
    if (project_instructions_.empty() && entries_.empty()) return {};

    std::ostringstream out;

    if (!project_instructions_.empty()) {
        out << "## Project Instructions\n";
        out << project_instructions_;
        if (!project_instructions_.empty() &&
            project_instructions_.back() != '\n') {
            out << '\n';
        }
        out << '\n';
    }

    if (!entries_.empty()) {
        out << "## Memory Context\n";
        for (const auto& entry : entries_) {
            out << "### " << entry.name;
            if (!entry.type.empty()) {
                out << " (" << entry.type << ")";
            }
            out << '\n';
            if (!entry.content.empty()) {
                out << entry.content;
                if (entry.content.back() != '\n') out << '\n';
            }
            out << '\n';
        }
    }

    return out.str();
}

}  // namespace omni::engine
