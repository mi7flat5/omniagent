#include "session_persistence.h"

#include "../core/message.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string usage_to_json_str(const Usage& u) {
    nlohmann::json j;
    j["input_tokens"]      = u.input_tokens;
    j["output_tokens"]     = u.output_tokens;
    j["cache_read_tokens"] = u.cache_read_tokens;
    return j.dump();
}

Usage usage_from_json(const nlohmann::json& j) {
    Usage u;
    u.input_tokens      = j.value("input_tokens",      int64_t{0});
    u.output_tokens     = j.value("output_tokens",     int64_t{0});
    u.cache_read_tokens = j.value("cache_read_tokens", int64_t{0});
    return u;
}

}  // namespace

// ---------------------------------------------------------------------------
// SessionPersistence
// ---------------------------------------------------------------------------

SessionPersistence::SessionPersistence(std::filesystem::path storage_dir)
    : storage_dir_(std::move(storage_dir))
{}

std::filesystem::path SessionPersistence::path_for(const std::string& id) const {
    return storage_dir_ / (id + ".jsonl");
}

void SessionPersistence::save(const SessionRecord& record) {
    std::filesystem::create_directories(storage_dir_);

    const std::filesystem::path p = path_for(record.id);
    std::ofstream ofs(p, std::ios::out | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("SessionPersistence::save: cannot open " + p.string());
    }

    // First line: metadata
    nlohmann::json meta;
    meta["id"]         = record.id;
    meta["created_at"] = record.created_at;
    meta["updated_at"] = record.updated_at;
    meta["usage"]["input_tokens"]      = record.total_usage.input_tokens;
    meta["usage"]["output_tokens"]     = record.total_usage.output_tokens;
    meta["usage"]["cache_read_tokens"] = record.total_usage.cache_read_tokens;
    ofs << meta.dump() << '\n';

    // One line per message
    for (const Message& msg : record.messages) {
        ofs << msg.to_json().dump() << '\n';
    }

    if (!ofs) {
        throw std::runtime_error("SessionPersistence::save: write error for " + p.string());
    }
}

std::optional<SessionRecord> SessionPersistence::load(const std::string& session_id) {
    const std::filesystem::path p = path_for(session_id);
    if (!std::filesystem::exists(p)) {
        return std::nullopt;
    }

    std::ifstream ifs(p);
    if (!ifs) {
        return std::nullopt;
    }

    SessionRecord record;
    std::string   line;

    // First line: metadata
    if (!std::getline(ifs, line) || line.empty()) {
        return std::nullopt;
    }
    {
        const nlohmann::json meta = nlohmann::json::parse(line);
        record.id         = meta.value("id",         std::string{});
        record.created_at = meta.value("created_at", std::string{});
        record.updated_at = meta.value("updated_at", std::string{});
        if (meta.contains("usage")) {
            record.total_usage = usage_from_json(meta["usage"]);
        }
    }

    // Remaining lines: messages
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        record.messages.push_back(Message::from_json(nlohmann::json::parse(line)));
    }

    return record;
}

std::vector<std::string> SessionPersistence::list(int limit) {
    if (!std::filesystem::exists(storage_dir_)) {
        return {};
    }

    // Collect (mtime, id) pairs
    using Entry = std::pair<std::filesystem::file_time_type, std::string>;
    std::vector<Entry> entries;

    for (const auto& de : std::filesystem::directory_iterator(storage_dir_)) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".jsonl") continue;

        const std::string stem = de.path().stem().string();
        const auto mtime = de.last_write_time();
        entries.emplace_back(mtime, stem);
    }

    // Sort newest first
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.first > b.first; });

    std::vector<std::string> result;
    result.reserve(std::min(static_cast<int>(entries.size()), limit));
    for (int i = 0; i < limit && i < static_cast<int>(entries.size()); ++i) {
        result.push_back(std::move(entries[i].second));
    }
    return result;
}

bool SessionPersistence::remove(const std::string& session_id) {
    const std::filesystem::path p = path_for(session_id);
    if (!std::filesystem::exists(p)) {
        return false;
    }
    std::filesystem::remove(p);
    return true;
}

}  // namespace omni::engine
