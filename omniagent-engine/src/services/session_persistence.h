#pragma once

#include <omni/types.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

struct SessionRecord {
    std::string          id;
    std::string          created_at;  // ISO 8601
    std::string          updated_at;
    std::vector<Message> messages;
    Usage                total_usage;
};

class SessionPersistence {
public:
    explicit SessionPersistence(std::filesystem::path storage_dir);

    /// Save a session to JSONL (one JSON object per line: metadata line + one per message).
    void save(const SessionRecord& record);

    /// Load a session by ID. Returns nullopt if not found.
    std::optional<SessionRecord> load(const std::string& session_id);

    /// List all session IDs, most recent first.
    std::vector<std::string> list(int limit = 50);

    /// Delete a session.
    bool remove(const std::string& session_id);

private:
    std::filesystem::path storage_dir_;
    std::filesystem::path path_for(const std::string& id) const;
};

}  // namespace omni::engine
