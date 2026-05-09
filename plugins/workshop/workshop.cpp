/*
 * workshop — rAthena plugin that runs Lua mods from a `mods/` folder.
 *
 * Each subfolder of plugins/workshop/mods/ is a mod, identified by its
 * modinfo.yml. The plugin loads modinfo.yml, runs each listed script
 * through Lua 5.4, and optionally calls the mod's on_init hook.
 *
 * Enable in conf/plugins.conf:
 *   plugins/workshop/workshop
 */

#include "workshop.hpp"
#include "lua_bridge.hpp"
#include "modloader.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

plugin_api_t* g_api = nullptr;

static plugin_info_t info = {
    "Workshop",
    "rAthena Workshop",
    "0.1.0",
    "Lua-driven mod loader. Reads YAML and Lua (source or .luac) from mods/."
};

PLUGIN_API plugin_info_t* plugin_info() { return &info; }

// ---- paths ------------------------------------------------------------

static std::string g_root;
static std::string g_mods;

const char* workshop_root() { return g_root.c_str(); }
const char* workshop_mods_dir() { return g_mods.c_str(); }

static bool dir_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_dir(const std::string& p) {
    if (dir_exists(p)) return true;
    return mkdir(p.c_str(), 0755) == 0;
}

// ---- logging ----------------------------------------------------------
//
// plugin_api_t exposes log.info/warning/error but each takes a single string.
// These helpers format with snprintf first so callers can use printf-style.

#define WLOG_DEFINE(NAME, FN)                                            \
    void NAME(const char* fmt, ...) {                                    \
        char buf[1024];                                                  \
        char prefixed[1056];                                             \
        va_list ap;                                                      \
        va_start(ap, fmt);                                               \
        vsnprintf(buf, sizeof(buf), fmt, ap);                            \
        va_end(ap);                                                      \
        snprintf(prefixed, sizeof(prefixed), "[workshop] %s", buf);      \
        if (g_api) g_api->log.FN(prefixed);                              \
        else fprintf(stderr, "%s\n", prefixed);                          \
    }

WLOG_DEFINE(wlog_info,    info)
WLOG_DEFINE(wlog_status,  status)
WLOG_DEFINE(wlog_warning, warning)
WLOG_DEFINE(wlog_error,   error)

#undef WLOG_DEFINE

// ---- script command: workshop_reload ----------------------------------
//
// Lets a GM reload all mods at runtime: `workshop_reload;` from a script.

static int32_t buildin_workshop_reload(script_state* st) {
    workshop::LuaBridge::instance().shutdown();
    workshop::LuaBridge::instance().init();
    workshop::register_globals(workshop::LuaBridge::instance().L());

    auto& loader = workshop::ModLoader::instance();
    bool ok = loader.discover(g_mods) && loader.load_all();
    g_api->script.pushint(st, ok ? 1 : 0);
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// ---- lifecycle --------------------------------------------------------

PLUGIN_API bool plugin_init(plugin_api_t* api) {
    g_api = api;

    // Map-server starts in the rAthena root, so plugin assets live at
    // ./plugins/workshop relative to cwd.
    g_root = "plugins/workshop";
    g_mods = g_root + "/mods";

    // Automation: create mods/ on first run (and scaffold an example mod
    // so users have something concrete to copy from).
    if (!ensure_dir(g_root)) {
        wlog_error("cannot access plugin root: %s", g_root.c_str());
        return false;
    }
    if (!dir_exists(g_mods)) {
        wlog_status("mods/ not found, creating %s", g_mods.c_str());
        if (!ensure_dir(g_mods)) {
            wlog_error("failed to create mods directory");
            return false;
        }
        workshop::scaffold_mods_dir(g_mods);
    }

    if (!workshop::LuaBridge::instance().init()) {
        wlog_error("failed to initialise Lua VM: %s",
                   workshop::LuaBridge::instance().last_error().c_str());
        return false;
    }
    workshop::register_globals(workshop::LuaBridge::instance().L());

    auto& loader = workshop::ModLoader::instance();
    if (!loader.discover(g_mods)) {
        wlog_warning("mod discovery had errors — see log above");
    }
    if (!loader.load_all()) {
        wlog_warning("some mods failed to load");
    }

    api->script_addcommand("workshop_reload", "", buildin_workshop_reload);

    wlog_status("plugin loaded — %zu mod(s) active", loader.mods().size());
    return true;
}

PLUGIN_API void plugin_final() {
    workshop::LuaBridge::instance().shutdown();
    wlog_status("plugin unloaded");
}
