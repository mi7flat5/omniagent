#pragma once

#include <omni/tool.h>
#include <memory>
#include <string>
#include <vector>

namespace omni::engine {

/// Plugin manifest — describes what a plugin provides.
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> tool_names;     // tools this plugin provides
    std::vector<std::string> command_names;  // commands this plugin provides
};

/// Plugin interface. Plugins implement this and export a factory function.
class Plugin {
public:
    virtual ~Plugin() = default;

    /// Get the plugin manifest.
    virtual PluginManifest manifest() const = 0;

    /// Create tools provided by this plugin.
    virtual std::vector<std::unique_ptr<Tool>> create_tools() = 0;
};

}  // namespace omni::engine

/// Factory function signature. Plugins export this as extern "C".
/// The engine calls this to create a Plugin instance.
extern "C" {
    typedef omni::engine::Plugin* (*omni_plugin_create_fn)();
    typedef void (*omni_plugin_destroy_fn)(omni::engine::Plugin*);
}

/// Plugins must export these symbols:
///   extern "C" omni::engine::Plugin* omni_plugin_create();
///   extern "C" void omni_plugin_destroy(omni::engine::Plugin* p);
