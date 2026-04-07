// plugin_manager.cpp — dynamic plugin loading via dlopen/LoadLibrary.
//
// NOTE: This file must be added to the omni-engine target source list in
// CMakeLists.txt under src/engine/ as:
//     src/plugins/plugin_manager.cpp

#include "plugin_manager.h"
#include "tools/tool_registry.h"

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

#include <stdexcept>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Platform helpers (file-local, not part of the public API)
// ---------------------------------------------------------------------------

namespace {

void* platform_open(const std::filesystem::path& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryW(path.wstring().c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* platform_sym(void* handle, const char* sym) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), sym));
#else
    return dlsym(handle, sym);
#endif
}

void platform_close(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

const char* platform_error() {
#ifdef _WIN32
    static thread_local char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, GetLastError(), 0, buf, sizeof(buf), nullptr);
    return buf;
#else
    return dlerror();
#endif
}

/// Return the native shared-library extension for the current platform.
const char* lib_extension() {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// PluginManager
// ---------------------------------------------------------------------------

PluginManager::PluginManager(ToolRegistry& registry) : registry_(registry) {}

PluginManager::~PluginManager() {
    std::lock_guard lock(mutex_);
    for (auto& [name, lp] : plugins_) {
        unregister_plugin_tools_locked(lp.manifest);
        destroy_loaded_locked(lp);
    }
    plugins_.clear();
}

bool PluginManager::load(const std::filesystem::path& library_path) {
    // Clear any stale dl error state.
    (void)platform_error();

    void* handle = platform_open(library_path);
    if (!handle) {
        // platform_error() is valid immediately after a failed open.
        return false;
    }

    // Resolve factory symbols.
    auto* create_fn = reinterpret_cast<omni_plugin_create_fn>(
        platform_sym(handle, "omni_plugin_create"));
    auto* destroy_fn = reinterpret_cast<omni_plugin_destroy_fn>(
        platform_sym(handle, "omni_plugin_destroy"));

    if (!create_fn || !destroy_fn) {
        platform_close(handle);
        return false;
    }

    Plugin* instance = create_fn();
    if (!instance) {
        platform_close(handle);
        return false;
    }

    PluginManifest mf = instance->manifest();
    if (mf.name.empty()) {
        destroy_fn(instance);
        platform_close(handle);
        return false;
    }

    std::lock_guard lock(mutex_);

    // If a plugin with this name is already loaded, reject the duplicate.
    if (plugins_.count(mf.name)) {
        destroy_fn(instance);
        platform_close(handle);
        return false;
    }

    register_plugin_tools_locked(*instance);

    LoadedPlugin lp;
    lp.name       = mf.name;
    lp.dl_handle  = handle;
    lp.instance   = instance;
    lp.destroy_fn = destroy_fn;
    lp.manifest   = std::move(mf);

    plugins_.emplace(lp.name, std::move(lp));
    return true;
}

int PluginManager::load_directory(const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir)) {
        return 0;
    }

    const std::string ext = lib_extension();
    int loaded = 0;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ext) continue;
        if (load(entry.path())) {
            ++loaded;
        }
    }
    return loaded;
}

bool PluginManager::unload(const std::string& name) {
    std::lock_guard lock(mutex_);

    auto it = plugins_.find(name);
    if (it == plugins_.end()) return false;

    unregister_plugin_tools_locked(it->second.manifest);
    destroy_loaded_locked(it->second);
    plugins_.erase(it);
    return true;
}

std::vector<PluginManifest> PluginManager::list() const {
    std::lock_guard lock(mutex_);
    std::vector<PluginManifest> result;
    result.reserve(plugins_.size());
    for (const auto& [name, lp] : plugins_) {
        result.push_back(lp.manifest);
    }
    return result;
}

size_t PluginManager::count() const {
    std::lock_guard lock(mutex_);
    return plugins_.size();
}

bool PluginManager::register_plugin(std::unique_ptr<Plugin> plugin) {
    if (!plugin) return false;

    PluginManifest mf = plugin->manifest();
    if (mf.name.empty()) return false;

    std::lock_guard lock(mutex_);

    if (plugins_.count(mf.name)) return false;

    register_plugin_tools_locked(*plugin);

    LoadedPlugin lp;
    lp.name       = mf.name;
    lp.dl_handle  = nullptr;           // not a dlopen plugin
    lp.instance   = plugin.release();  // take ownership
    lp.destroy_fn = nullptr;           // delete directly in destroy_loaded_locked
    lp.manifest   = std::move(mf);

    plugins_.emplace(lp.name, std::move(lp));
    return true;
}

// ---------------------------------------------------------------------------
// Private helpers (caller holds mutex_)
// ---------------------------------------------------------------------------

void PluginManager::register_plugin_tools_locked(Plugin& plugin) {
    // create_tools() is called once here; tools are moved into the registry.
    auto tools = plugin.create_tools();
    for (auto& tool : tools) {
        if (tool) {
            registry_.register_tool(std::move(tool));
        }
    }
}

void PluginManager::unregister_plugin_tools_locked(const PluginManifest& manifest) {
    for (const auto& tool_name : manifest.tool_names) {
        registry_.unregister_tool(tool_name);
    }
}

void PluginManager::destroy_loaded_locked(LoadedPlugin& lp) noexcept {
    if (lp.instance) {
        if (lp.destroy_fn) {
            lp.destroy_fn(lp.instance);
        } else {
            delete lp.instance;
        }
        lp.instance = nullptr;
    }
    if (lp.dl_handle) {
        platform_close(lp.dl_handle);
        lp.dl_handle = nullptr;
    }
}

}  // namespace omni::engine
