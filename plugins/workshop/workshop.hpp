#ifndef WORKSHOP_PLUGIN_HPP
#define WORKSHOP_PLUGIN_HPP

#include <map/plugin.hpp>

extern plugin_api_t* g_api;

// Workshop plugin runtime root (resolves to <cwd>/plugins/workshop relative to
// the running map-server, which is launched from the rAthena root).
const char* workshop_root();

// Path to the mods directory (workshop_root()/mods). Created if missing.
const char* workshop_mods_dir();

// Logging helpers — prefix every line with [workshop] so server logs stay tidy.
void wlog_info   (const char* fmt, ...);
void wlog_status (const char* fmt, ...);
void wlog_warning(const char* fmt, ...);
void wlog_error  (const char* fmt, ...);

#endif
