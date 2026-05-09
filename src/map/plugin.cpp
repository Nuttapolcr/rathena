// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "plugin.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef HMODULE plugin_handle_t;
   static plugin_handle_t open_plugin(const char* path) { return LoadLibraryA(path); }
   static void* sym_plugin(plugin_handle_t h, const char* s) { return reinterpret_cast<void*>(GetProcAddress(h, s)); }
   static void close_plugin(plugin_handle_t h) { FreeLibrary(h); }
#  define PLUGIN_EXT ".dll"
#else
#  include <dlfcn.h>
   typedef void* plugin_handle_t;
   static plugin_handle_t open_plugin(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
   static void* sym_plugin(plugin_handle_t h, const char* s) { return dlsym(h, s); }
   static void close_plugin(plugin_handle_t h) { dlclose(h); }
#  define PLUGIN_EXT ".so"
#endif

// rathena server headers — provides access to all map-server functions
#include <common/cbasetypes.hpp>
#include <common/malloc.hpp>
#include <common/mapindex.hpp>
#include <common/showmsg.hpp>

#include <common/timer.hpp>

#include "atcommand.hpp"
#include "clif.hpp"
#include "homunculus.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "pc.hpp"
#include "quest.hpp"
#include "script.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "storage.hpp"
#include "unit.hpp"

using namespace rathena;

// push_val2 and push_str are defined in script.cpp without a header declaration.
// We forward-declare them here so plugin.cpp can call them for script push wrappers.
struct script_data* push_val2(struct script_stack* stack, enum c_op type, int64 val, struct reg_db* ref);
struct script_data* push_str(struct script_stack* stack, enum c_op type, char* str);

// ============================================================
// Hook chain and plugin registry
// ============================================================

struct HookEntry {
	plugin_hook_cb cb;
	void*          user_data;
	int            priority;
};

struct PluginCmd {
	std::string        name;
	std::string        arg;
	plugin_script_func func;
};

struct LoadedPlugin {
	plugin_handle_t handle;
	plugin_info_t*  info;
	void (*pfn_final)();
};

static std::vector<HookEntry>    hook_chains[HOOK_MAX];
static std::vector<PluginCmd>    plugin_cmds;
static std::vector<LoadedPlugin> loaded_plugins;

// ============================================================
// Hook management API implementation
// ============================================================

static void impl_hook_add(int type, plugin_hook_cb cb, void* udata, int prio)
{
	if (type < 0 || type >= HOOK_MAX || !cb)
		return;
	HookEntry e = { cb, udata, prio };
	auto& chain = hook_chains[type];
	auto it = std::lower_bound(chain.begin(), chain.end(), e,
		[](const HookEntry& a, const HookEntry& b){ return a.priority < b.priority; });
	chain.insert(it, e);
}

static void impl_hook_remove(int type, plugin_hook_cb cb)
{
	if (type < 0 || type >= HOOK_MAX || !cb)
		return;
	auto& chain = hook_chains[type];
	chain.erase(std::remove_if(chain.begin(), chain.end(),
		[cb](const HookEntry& e){ return e.cb == cb; }), chain.end());
}

static bool impl_script_addcommand(const char* name, const char* arg, plugin_script_func func)
{
	if (!name || !func)
		return false;
	int idx = static_cast<int>(plugin_cmds.size());
	plugin_cmds.push_back({ name, arg ? arg : "*", func });
	return script_plugin_register(name, arg ? arg : "*", func, idx);
}

// ============================================================
// Script state API wrappers
// ============================================================

static bool api_script_hasdata(script_state* st, int n)
{
	return script_hasdata(st, n) != 0;
}

static int64_t api_script_getnum(script_state* st, int n)
{
	return static_cast<int64_t>(conv_num(st, script_getdata(st, n)));
}

static const char* api_script_getstr(script_state* st, int n)
{
	return conv_str(st, script_getdata(st, n));
}

static void api_script_pushint(script_state* st, int64_t val)
{
	push_val2(st->stack, C_INT, static_cast<int64>(val), nullptr);
}

static void api_script_pushstr(script_state* st, const char* val)
{
	push_str(st->stack, C_STR, aStrdup(val));
}

static map_session_data* api_script_rid2sd(script_state* st)
{
	return map_id2sd(st->rid);
}

// ============================================================
// PC API wrappers
// ============================================================

static void api_pc_message(int32_t fd, const char* msg)
{
	clif_displaymessage(fd, msg);
}

static int api_pc_additem(map_session_data* sd, const plugin_item_t* pit, int32_t amount, int log_type)
{
	if (!pit) return 1;
	item it = {};
	it.nameid   = static_cast<t_itemid>(pit->nameid);
	it.identify = pit->identify;
	return static_cast<int>(pc_additem(sd, &it, amount,
	                                   static_cast<e_log_pick_type>(log_type)));
}

static char api_pc_delitem(map_session_data* sd, int32_t n, int32_t amount,
                           int32_t type, int16_t reason, int log_type)
{
	return pc_delitem(sd, n, amount, type, reason,
	                  static_cast<e_log_pick_type>(log_type));
}

static void api_pc_gainexp(map_session_data* sd, block_list* src,
                           uint64_t base_exp, uint64_t job_exp, uint8_t exp_flag)
{
	pc_gainexp(sd, src,
	           static_cast<t_exp>(base_exp),
	           static_cast<t_exp>(job_exp),
	           exp_flag);
}

static char api_pc_payzeny(map_session_data* sd, int32_t zeny, int log_type)
{
	return pc_payzeny(sd, zeny, static_cast<e_log_pick_type>(log_type));
}

static char api_pc_getzeny(map_session_data* sd, int32_t zeny, int log_type)
{
	return pc_getzeny(sd, zeny, static_cast<e_log_pick_type>(log_type));
}

static int api_pc_setpos(map_session_data* sd, uint16_t mapidx,
                         int32_t x, int32_t y, int clrtype)
{
	return static_cast<int>(pc_setpos(sd, mapidx, x, y,
	                                  static_cast<clr_type>(clrtype)));
}

static int32_t     api_pc_get_fd   (map_session_data* sd) { return sd ? sd->fd : -1; }
static int32_t     api_pc_get_aid  (map_session_data* sd) { return sd ? static_cast<int32_t>(sd->status.account_id) : 0; }
static const char* api_pc_get_name (map_session_data* sd) { return sd ? sd->status.name : ""; }
static uint32_t    api_pc_get_blv  (map_session_data* sd) { return sd ? sd->status.base_level : 0; }
static uint32_t    api_pc_get_jlv  (map_session_data* sd) { return sd ? sd->status.job_level : 0; }
static int16_t     api_pc_get_mapid(map_session_data* sd) { return sd ? sd->m : 0; }
static int16_t     api_pc_get_pos_x(map_session_data* sd) { return sd ? sd->x : 0; }
static int16_t     api_pc_get_pos_y(map_session_data* sd) { return sd ? sd->y : 0; }
static block_list* api_pc_as_bl    (map_session_data* sd) { return static_cast<block_list*>(sd); }

// ============================================================
// Mob API wrappers
// ============================================================

static int32_t api_mob_once_spawn(map_session_data* sd,
                                  int16_t m, int16_t x, int16_t y,
                                  const char* mobname, int32_t mob_id, int32_t amount,
                                  const char* event, uint32_t size, int ai)
{
	return mob_once_spawn(sd, m, x, y, mobname, mob_id, amount, event,
	                      size, static_cast<mob_ai>(ai));
}

static int32_t     api_mob_get_id  (mob_data* md) { return md ? static_cast<int32_t>(md->mob_id) : 0; }
static int16_t     api_mob_get_x   (mob_data* md) { return md ? md->x : 0; }
static int16_t     api_mob_get_y   (mob_data* md) { return md ? md->y : 0; }
static const char* api_mob_get_name(mob_data* md) { return md ? md->name : ""; }

// ============================================================
// Map API wrappers
// ============================================================

static uint16_t api_map_name2id(const char* mapname)
{
	return mapindex_name2id(mapname);
}

static const char* api_map_id2name(uint16_t mapidx)
{
	return mapindex_id2name(mapidx);
}

static map_session_data* api_map_id2sd(int32_t id)
{
	return map_id2sd(id);
}

static map_session_data* api_map_charid2sd(int32_t charid)
{
	return map_charid2sd(charid);
}

static map_session_data* api_map_nick2sd(const char* nick, bool allow_partial)
{
	return map_nick2sd(nick, allow_partial);
}

// ============================================================
// Status API wrappers
// ============================================================

static int32_t api_status_heal(block_list* bl, int64_t hp, int64_t sp, int32_t flag)
{
	return status_heal(bl, static_cast<int64>(hp), static_cast<int64>(sp), flag);
}

static int32_t api_status_damage(block_list* src, block_list* target,
                                 int64_t hp, int64_t sp, int64_t walkdelay,
                                 int32_t flag, uint16_t skill_id)
{
	return status_damage(src, target,
	                     static_cast<int64>(hp), static_cast<int64>(sp),
	                     static_cast<t_tick>(walkdelay), flag, skill_id);
}

// ============================================================
// Block list API wrappers
// ============================================================

static int32_t api_bl_get_type(block_list* bl)
{
	return bl ? static_cast<int32_t>(bl->type) : 0;
}

static map_session_data* api_bl_as_sd(block_list* bl)
{
	return (bl && bl->type == BL_PC) ? static_cast<map_session_data*>(bl) : nullptr;
}

// ============================================================
// Item API wrappers
// ============================================================

static uint32_t api_item_get_nameid(item* it)
{
	return it ? static_cast<uint32_t>(it->nameid) : 0;
}

// ============================================================
// Atcmd API wrappers
// ============================================================

static bool api_atcmd_register(const char* name, int level, plugin_atcmd_func func)
{
	return atcommand_plugin_register(name, level, func);
}

// ============================================================
// Quest API wrappers
// ============================================================

static int32_t api_quest_add(map_session_data* sd, int32_t quest_id)
{
	return quest_add(sd, quest_id);
}

static int32_t api_quest_update_status(map_session_data* sd, int32_t quest_id, int status)
{
	return quest_update_status(sd, quest_id, static_cast<e_quest_state>(status));
}

static int32_t api_quest_check(const map_session_data* sd, int32_t quest_id, int type)
{
	return quest_check(sd, quest_id, static_cast<e_quest_check_type>(type));
}

// ============================================================
// NPC API wrappers
// ============================================================

static int api_npc_event(map_session_data* sd, const char* event_name, int ontouch)
{
	return npc_event(sd, event_name, ontouch);
}

// ============================================================
// Skill API wrappers
// ============================================================

static int32_t api_skill_get_lv(map_session_data* sd, uint16_t skill_id)
{
	return static_cast<int32_t>(pc_checkskill(sd, skill_id));
}

static int32_t api_skill_use_id(map_session_data* sd, uint16_t skill_id,
                                uint16_t skill_lv, int32_t target_id)
{
	block_list* bl = static_cast<block_list*>(sd);
	return unit_skilluse_id(bl, target_id, skill_id, skill_lv);
}

// ============================================================
// Storage API wrappers
// ============================================================

static int32_t api_storage_open(map_session_data* sd)
{
	return storage_storageopen(sd);
}

// ============================================================
// Map iteration wrappers
// ============================================================

// Trampoline that forwards va_list-style block_list iteration into the
// plugin's plain-pointer callback signature.
static int32 plugin_blcb_trampoline(block_list* bl, va_list ap)
{
	plugin_blcb cb = va_arg(ap, plugin_blcb);
	void*       ud = va_arg(ap, void*);
	return cb(bl, ud);
}

static int32_t api_map_foreachinmap(plugin_blcb cb, void* user, int16_t m, int32_t type_mask)
{
	if (!cb) return 0;
	return map_foreachinmap(plugin_blcb_trampoline, m, type_mask, cb, user);
}

static int32_t api_map_foreachinarea(plugin_blcb cb, void* user, int16_t m,
                                     int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     int32_t type_mask)
{
	if (!cb) return 0;
	return map_foreachinarea(plugin_blcb_trampoline, m, x0, y0, x1, y1, type_mask, cb, user);
}

static int32_t api_map_get_mapflag(int16_t m, int32_t flag)
{
	return map_getmapflag_sub(m, static_cast<e_mapflag>(flag), nullptr);
}

// ============================================================
// Item DB wrappers
// ============================================================

static bool api_item_db_exists(uint32_t nameid)
{
	return item_db.exists(static_cast<t_itemid>(nameid));
}

static const char* api_item_db_get_name(uint32_t nameid)
{
	auto id = item_db.find(static_cast<t_itemid>(nameid));
	return id ? id->name.c_str() : "";
}

static const char* api_item_db_get_ename(uint32_t nameid)
{
	auto id = item_db.find(static_cast<t_itemid>(nameid));
	return id ? id->ename.c_str() : "";
}

static int32_t api_item_db_get_type(uint32_t nameid)
{
	auto id = item_db.find(static_cast<t_itemid>(nameid));
	return id ? static_cast<int32_t>(id->type) : -1;
}

// ============================================================
// Skill DB wrappers
// ============================================================

static const char* api_skill_get_name(uint16_t skill_id)
{
	const char* n = skill_get_name(skill_id);
	return n ? n : "";
}

static int32_t api_skill_get_inf(uint16_t skill_id)
{
	return skill_get_inf(skill_id);
}

static uint16_t api_skill_name2id(const char* name)
{
	return name ? skill_name2id(name) : 0;
}

// ============================================================
// Clif wrappers
// ============================================================

static void api_clif_displaymessage(int32_t fd, const char* msg)
{
	if (msg) clif_displaymessage(fd, msg);
}

static void api_clif_emotion(block_list* bl, int32_t emote)
{
	if (bl) clif_emotion(*bl, static_cast<emotion_type>(emote));
}

static void api_clif_specialeffect(block_list* bl, int32_t effect_id, int32_t target)
{
	if (bl) clif_specialeffect(bl, effect_id, static_cast<send_target>(target));
}

static void api_clif_specialeffect_single(block_list* bl, int32_t effect_id, int32_t fd)
{
	if (bl) clif_specialeffect_single(bl, effect_id, fd);
}

static void api_clif_progressbar(map_session_data* sd, uint32_t color, uint32_t seconds)
{
	if (sd) clif_progressbar(sd, color, seconds);
}

static void api_clif_progressbar_abort(map_session_data* sd)
{
	if (sd) clif_progressbar_abort(sd);
}

static void api_clif_broadcast(block_list* bl, const char* msg, int32_t type, int32_t target)
{
	if (!msg) return;
	clif_broadcast(bl, msg, std::strlen(msg) + 1, type, static_cast<send_target>(target));
}

static void api_clif_messagecolor(block_list* bl, uint32_t color, const char* msg,
                                  bool rgb2bgr, int32_t target)
{
	if (!msg) return;
	clif_messagecolor_target(bl, color, msg, rgb2bgr,
	                         static_cast<send_target>(target), nullptr);
}

// ============================================================
// Timer wrappers
// ============================================================

static int64_t api_timer_gettick(void)
{
	return static_cast<int64_t>(gettick());
}

static int32_t api_timer_add_timer(int64_t tick, plugin_timer_func func,
                                   int32_t id, intptr_t data)
{
	if (!func) return -1;
	return add_timer(static_cast<t_tick>(tick),
	                 reinterpret_cast<TimerFunc>(func), id, data);
}

static int32_t api_timer_add_timer_interval(int64_t tick, plugin_timer_func func,
                                            int32_t id, intptr_t data, int32_t interval_ms)
{
	if (!func) return -1;
	return add_timer_interval(static_cast<t_tick>(tick),
	                          reinterpret_cast<TimerFunc>(func),
	                          id, data, interval_ms);
}

static int32_t api_timer_delete_timer(int32_t tid, plugin_timer_func func)
{
	if (!func) return -1;
	return delete_timer(tid, reinterpret_cast<TimerFunc>(func));
}

// ============================================================
// Log (ShowXxx) wrappers
// ============================================================
// Plugins pass already-formatted strings; we forward as a single "%s" to
// avoid format-string issues across DLL boundaries.

static void api_log_info   (const char* msg) { ShowInfo   ("%s\n", msg ? msg : ""); }
static void api_log_status (const char* msg) { ShowStatus ("%s\n", msg ? msg : ""); }
static void api_log_warning(const char* msg) { ShowWarning("%s\n", msg ? msg : ""); }
static void api_log_error  (const char* msg) { ShowError  ("%s\n", msg ? msg : ""); }
static void api_log_debug  (const char* msg) { ShowDebug  ("%s\n", msg ? msg : ""); }

// ============================================================
// Main API struct — handed to every plugin on init
// ============================================================

static plugin_api_t s_api = {
	// Hook management
	impl_hook_add,
	impl_hook_remove,
	impl_script_addcommand,

	// script sub-struct
	{
		api_script_hasdata,
		api_script_getnum,
		api_script_getstr,
		api_script_pushint,
		api_script_pushstr,
		api_script_rid2sd,
	},

	// pc sub-struct
	{
		api_pc_message,
		api_pc_additem,
		api_pc_delitem,
		api_pc_gainexp,
		api_pc_payzeny,
		api_pc_getzeny,
		api_pc_setpos,
		api_pc_get_fd,
		api_pc_get_aid,
		api_pc_get_name,
		api_pc_get_blv,
		api_pc_get_jlv,
		api_pc_get_mapid,
		api_pc_get_pos_x,
		api_pc_get_pos_y,
		api_pc_as_bl,
	},

	// mob sub-struct
	{
		api_mob_once_spawn,
		api_mob_get_id,
		api_mob_get_x,
		api_mob_get_y,
		api_mob_get_name,
	},

	// map sub-struct
	{
		api_map_name2id,
		api_map_id2name,
		api_map_id2sd,
		api_map_charid2sd,
		api_map_nick2sd,
		api_map_foreachinmap,
		api_map_foreachinarea,
		api_map_get_mapflag,
	},

	// status sub-struct
	{
		api_status_heal,
		api_status_damage,
	},

	// bl sub-struct
	{
		api_bl_get_type,
		api_bl_as_sd,
	},

	// item_api sub-struct
	{
		api_item_get_nameid,
		api_item_db_exists,
		api_item_db_get_name,
		api_item_db_get_ename,
		api_item_db_get_type,
	},

	// atcmd sub-struct
	{
		api_atcmd_register,
	},

	// quest sub-struct
	{
		api_quest_add,
		api_quest_update_status,
		api_quest_check,
	},

	// npc sub-struct
	{
		api_npc_event,
	},

	// skill sub-struct
	{
		api_skill_get_lv,
		api_skill_use_id,
		api_skill_get_name,
		api_skill_get_inf,
		api_skill_name2id,
	},

	// storage sub-struct
	{
		api_storage_open,
	},

	// clif sub-struct
	{
		api_clif_displaymessage,
		api_clif_emotion,
		api_clif_specialeffect,
		api_clif_specialeffect_single,
		api_clif_progressbar,
		api_clif_progressbar_abort,
		api_clif_broadcast,
		api_clif_messagecolor,
	},

	// timer sub-struct
	{
		api_timer_gettick,
		api_timer_add_timer,
		api_timer_add_timer_interval,
		api_timer_delete_timer,
	},

	// log sub-struct
	{
		api_log_info,
		api_log_status,
		api_log_warning,
		api_log_error,
		api_log_debug,
	},
};

// ============================================================
// Public: fire hook chain
// ============================================================

int plugin_hook_fire(int type, void* data)
{
	if (type < 0 || type >= HOOK_MAX)
		return HOOK_CONTINUE;
	for (auto& e : hook_chains[type]) {
		if (e.cb(data, e.user_data) == HOOK_STOP)
			return HOOK_STOP;
	}
	return HOOK_CONTINUE;
}

// ============================================================
// Public: arg string for a plugin-registered script command
// ============================================================

const char* plugin_get_cmd_arg(int idx)
{
	if (idx >= 0 && idx < static_cast<int>(plugin_cmds.size()))
		return plugin_cmds[idx].arg.c_str();
	return "*";
}

// ============================================================
// Plugin loading
// ============================================================

static bool load_plugin(const std::string& path)
{
	plugin_handle_t handle = open_plugin(path.c_str());
	if (!handle) {
#ifdef _WIN32
		ShowError("plugin: Failed to load '%s' (error %lu)\n", path.c_str(), GetLastError());
#else
		ShowError("plugin: Failed to load '%s': %s\n", path.c_str(), dlerror());
#endif
		return false;
	}

	typedef plugin_info_t* (*pfn_info_t)();
	typedef bool           (*pfn_init_t)(plugin_api_t*);
	typedef void           (*pfn_final_t)();

	auto pfn_info  = reinterpret_cast<pfn_info_t> (sym_plugin(handle, "plugin_info"));
	auto pfn_init  = reinterpret_cast<pfn_init_t> (sym_plugin(handle, "plugin_init"));
	auto pfn_final = reinterpret_cast<pfn_final_t>(sym_plugin(handle, "plugin_final"));

	if (!pfn_info || !pfn_init) {
		ShowError("plugin: '%s' is missing required exports (plugin_info / plugin_init)\n",
		          path.c_str());
		close_plugin(handle);
		return false;
	}

	plugin_info_t* info = pfn_info();
	if (!info) {
		ShowError("plugin: '%s' returned null from plugin_info()\n", path.c_str());
		close_plugin(handle);
		return false;
	}

	if (!pfn_init(&s_api)) {
		ShowError("plugin: '%s' (%s) failed to initialize\n", path.c_str(), info->name);
		close_plugin(handle);
		return false;
	}

	loaded_plugins.push_back({ handle, info, pfn_final });
	ShowStatus("plugin: Loaded '" CL_WHITE "%s" CL_RESET "' v%s by %s\n",
	           info->name, info->version, info->author);
	return true;
}

// ============================================================
// Public: init / final
// ============================================================

void plugin_manager_init(void)
{
	std::ifstream conf("conf/plugins.conf");
	if (!conf.is_open())
		return;

	int loaded = 0;
	std::string line;
	while (std::getline(conf, line)) {
		size_t s = line.find_first_not_of(" \t\r\n");
		if (s == std::string::npos) continue;
		line = line.substr(s);
		if (line.empty() || line[0] == '#' || line[0] == '/') continue;
		size_t e = line.find_last_not_of(" \t\r\n");
		if (e != std::string::npos) line = line.substr(0, e + 1);
		if (line.empty()) continue;

		if (line.find('.') == std::string::npos)
			line += PLUGIN_EXT;

		if (load_plugin(line))
			++loaded;
	}

	if (loaded > 0)
		ShowStatus("plugin: %d plugin(s) loaded.\n", loaded);
}

void plugin_manager_final(void)
{
	// Clear plugin @commands before unloading DLLs — prevents dangling function pointers
	atcommand_plugin_final();

	for (auto it = loaded_plugins.rbegin(); it != loaded_plugins.rend(); ++it) {
		if (it->pfn_final)
			it->pfn_final();
		close_plugin(it->handle);
	}
	loaded_plugins.clear();
	plugin_cmds.clear();
	for (int i = 0; i < HOOK_MAX; ++i)
		hook_chains[i].clear();
}
