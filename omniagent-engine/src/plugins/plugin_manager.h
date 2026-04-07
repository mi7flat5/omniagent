#pragma once

#include <omni/plugin.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

class ToolRegistry;

struct LoadedPlugin {
    std::string name;
    void*                    dl_handle  = nullptr;  // dlopen handle (nullptr for in-process)
    Plugin*                  instance   = nullptr;  // created by factory or direct registration
    omni_plugin_destroy_fn   destroy_fn = nullptr;  // nullptr means delete directly
    PluginManifest           manifest;
};

class PluginManager {
public:
    explicit PluginManager(ToolRegistry& registry);
    ~PluginManager();

    /// Load a single plugin from a shared library path.
    bool load(const std::filesystem::path& library_path);

    /// Discover and load all plugins from a directory.
    /// Looks for .so (Linux), .dylib (macOS), .dll (Windows) files.
    int load_directory(const std::filesystem::path& dir);

    /// Unload a plugin by name.
    bool unload(const std::string& name);

    /// List loaded plugins.
    std::vector<PluginManifest> list() const;

    /// Number of loaded plugins.
    size_t count() const;

    /// Register a plugin instance directly (no dlopen). Useful for testing
    /// and for built-in plugins that are linked statically.
    /// Takes ownership of the plugin.
    bool register_plugin(std::unique_ptr<Plugin> plugin);

private:
    ToolRegistry&   registry_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LoadedPlugin> plugins_;

    // Must be called with mutex_ held.
    void register_plugin_tools_locked(Plugin& plugin);
    // Must be called with mutex_ held.
    void unregister_plugin_tools_locked(const PluginManifest& manifest);
    // Destroy and close a loaded plugin entry (no lock required — caller holds it).
    void destroy_loaded_locked(LoadedPlugin& lp) noexcept;
};

}  // namespace omni::engine
