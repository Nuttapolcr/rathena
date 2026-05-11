// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "plugin.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

#include <common/socket.hpp>
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

// get_val2_num / get_val2_str also live in script.cpp without a header.
int64       get_val2_num(struct script_state* st, int64 uid, struct reg_db* ref);
const char* get_val2_str(struct script_state* st, int64 uid, struct reg_db* ref);

// packetdb_addpacket is defined in clif.cpp without a header declaration.
void packetdb_addpacket(uint16 cmd, uint16 length,
                        void (*func)(int32, map_session_data*), ...);

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
	void*              user_data;
};

struct LoadedPlugin {
	plugin_handle_t handle;
	plugin_info_t*  info;
	void (*pfn_final)();
};

static std::vector<HookEntry>    hook_chains[HOOK_MAX];
static std::vector<PluginCmd>    plugin_cmds;
static std::vector<LoadedPlugin> loaded_plugins;

// Tracks every script_state currently parked via api_script_suspend.
// `script_free_state` calls plugin_script_state_freed() to drop entries
// when the engine reclaims a state, so resume() on a stale token is safe.
static std::unordered_set<script_state*> suspended_states;

// Tracks every packet id currently mapped to a plugin-supplied handler,
// along with the per-registration {func, user_data} closure. The engine
// installs `plugin_packet_trampoline` (below) into packet_db, and the
// trampoline looks up this map to invoke the real plugin function with
// its user_data.
struct PluginPacketEntry {
	plugin_packet_func func;
	void*              user_data;
};
static std::unordered_map<uint16_t, PluginPacketEntry> plugin_packet_handlers;

// ---- Plugin status changes ----
// Definitions are indexed by the id returned from sc.register_sc().
struct PluginSCDef {
	std::string         name;
	int32_t             calc_flag;   // OR of e_plugin_scb
	int32_t             icon;         // EFST_* or 0
	plugin_sc_calc_func calc;
	void*               user_data;
};
static std::vector<PluginSCDef> plugin_sc_defs;

// Active instances live in a side table keyed on bl->id (not on the
// status_change map, which is bounds-checked against SC_MAX everywhere).
struct PluginSCActive {
	int32_t sc_id;
	int32_t val1, val2, val3, val4;
	int32_t timer;       // INVALID_TIMER if permanent
};
static std::unordered_map<int32_t /*bl_id*/, std::vector<PluginSCActive>> plugin_sc_active;

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

static bool impl_script_addcommand(const char* name, const char* arg,
                                   plugin_script_func func, void* user_data)
{
	if (!name || !func)
		return false;
	int idx = static_cast<int>(plugin_cmds.size());
	plugin_cmds.push_back({ name, arg ? arg : "*", func, user_data });
	return script_plugin_register(name, arg ? arg : "*", idx);
}

// Called by the script engine's plugin trampoline (see script.cpp).
// Invokes the actual plugin function with its captured user_data.
int32_t plugin_dispatch_script_cmd(int idx, script_state* st)
{
	if (idx < 0 || idx >= static_cast<int>(plugin_cmds.size()))
		return PLUGIN_SCRIPT_CMD_FAILURE;
	auto& c = plugin_cmds[idx];
	return c.func(st, c.user_data);
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

static void* api_script_suspend(script_state* st)
{
	if (!st) return nullptr;

	// Park the script at the instruction after the current command.
	// run_script_main exits its RUN loop and falls into the
	// "state != END && rid" branch, leaving the state attached to the
	// player and waiting for someone to call run_script_main(st) again.
	st->state = STOP;
	suspended_states.insert(st);
	return st;
}

static void api_script_resume(void* token)
{
	if (!token) return;
	auto* st = static_cast<script_state*>(token);

	// Validate: only resume states we actually parked. If the engine
	// already freed `st` (player logout, NPC reload, etc.),
	// plugin_script_state_freed will have removed it from the set.
	auto it = suspended_states.find(st);
	if (it == suspended_states.end()) return;
	suspended_states.erase(it);

	run_script_main(st);
}

// ---- Variable storage wrappers ----
// Translate (varname, index) into the engine's uid form and forward to
// the existing setd_sub_* / get_val2_* helpers.

static void api_script_set_var_num(script_state* st, map_session_data* sd,
                                   const char* varname, int32_t index, int64_t value)
{
	if (!varname) return;
	setd_sub_num(st, sd, varname, index, static_cast<int64>(value), nullptr);
}

static void api_script_set_var_str(script_state* st, map_session_data* sd,
                                   const char* varname, int32_t index, const char* value)
{
	if (!varname) return;
	setd_sub_str(st, sd, varname, index, value ? value : "", nullptr);
}

static int64_t api_script_get_var_num(script_state* st, map_session_data* /*sd*/,
                                      const char* varname, int32_t index)
{
	if (!st || !varname) return 0;
	int64 uid = reference_uid(add_str(varname), index);
	return static_cast<int64_t>(get_val2_num(st, uid, nullptr));
}

static const char* api_script_get_var_str(script_state* st, map_session_data* /*sd*/,
                                          const char* varname, int32_t index)
{
	if (!st || !varname) return "";
	int64 uid = reference_uid(add_str(varname), index);
	return get_val2_str(st, uid, nullptr);
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

// ---- Stat bonus wrappers ----
static void api_pc_bonus(map_session_data* sd, int32_t type, int32_t val)
{
	if (sd) pc_bonus(sd, type, val);
}
static void api_pc_bonus2(map_session_data* sd, int32_t type, int32_t v1, int32_t v2)
{
	if (sd) pc_bonus2(sd, type, v1, v2);
}
static void api_pc_bonus3(map_session_data* sd, int32_t type,
                          int32_t v1, int32_t v2, int32_t v3)
{
	if (sd) pc_bonus3(sd, type, v1, v2, v3);
}
static void api_pc_bonus4(map_session_data* sd, int32_t type,
                          int32_t v1, int32_t v2, int32_t v3, int32_t v4)
{
	if (sd) pc_bonus4(sd, type, v1, v2, v3, v4);
}
static void api_pc_bonus5(map_session_data* sd, int32_t type,
                          int32_t v1, int32_t v2, int32_t v3, int32_t v4, int32_t v5)
{
	if (sd) pc_bonus5(sd, type, v1, v2, v3, v4, v5);
}

// ---- Inventory / equip / param accessors ----
static int32_t api_pc_countitem(map_session_data* sd, uint32_t nameid)
{
	if (!sd || !nameid) return 0;
	int32_t count = 0;
	for (int i = 0; i < MAX_INVENTORY; ++i) {
		const item& it = sd->inventory.u.items_inventory[i];
		if (it.nameid == static_cast<t_itemid>(nameid))
			count += it.amount;
	}
	return count;
}

static int64_t api_pc_read_param(map_session_data* sd, int32_t type)
{
	return sd ? static_cast<int64_t>(pc_readparam(sd, type)) : 0;
}

static uint32_t api_pc_get_equip_nameid(map_session_data* sd, int32_t equip_index)
{
	if (!sd || !equip_index_check(equip_index)) return 0;
	int16 inv_idx = pc_checkequip(sd, equip_bitmask[equip_index]);
	if (inv_idx < 0 || inv_idx >= MAX_INVENTORY) return 0;
	return static_cast<uint32_t>(sd->inventory.u.items_inventory[inv_idx].nameid);
}

// ---- NPC dialog response accessors ----
static int32_t     api_pc_get_npc_id    (map_session_data* sd) { return sd ? sd->npc_id     : 0; }
static int32_t     api_pc_get_npc_menu  (map_session_data* sd) { return sd ? sd->npc_menu   : 0; }
static int32_t     api_pc_get_npc_amount(map_session_data* sd) { return sd ? sd->npc_amount : 0; }
static const char* api_pc_get_npc_str   (map_session_data* sd) { return sd ? sd->npc_str    : ""; }

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

static inline bool sc_type_in_range(int32_t type)
{
	return type > SC_NONE && type < SC_MAX;
}

static bool api_status_change_start(block_list* src, block_list* bl,
                                    int32_t type, int32_t rate,
                                    int32_t val1, int32_t val2,
                                    int32_t val3, int32_t val4,
                                    int64_t duration_ms, int32_t flag)
{
	if (!bl || !sc_type_in_range(type))
		return false;
	return status_change_start(src, bl, static_cast<sc_type>(type), rate,
	                           val1, val2, val3, val4,
	                           static_cast<t_tick>(duration_ms),
	                           static_cast<uint8>(flag));
}

static int32_t api_status_change_end(block_list* bl, int32_t type)
{
	if (!bl || !sc_type_in_range(type))
		return 0;
	return status_change_end(bl, static_cast<sc_type>(type));
}

static void api_status_change_clear(block_list* bl, int32_t type)
{
	if (!bl) return;
	status_change_clear(bl, type);
}

static bool api_status_has_change(block_list* bl, int32_t type)
{
	if (!bl || !sc_type_in_range(type))
		return false;
	status_change* sc = status_get_sc(bl);
	return sc && sc->getSCE(static_cast<sc_type>(type)) != nullptr;
}

static int32_t api_status_change_val(block_list* bl, int32_t type, int32_t which)
{
	if (!bl || !sc_type_in_range(type) || which < 1 || which > 4)
		return 0;
	status_change* sc = status_get_sc(bl);
	if (!sc) return 0;
	const status_change_entry* sce = sc->getSCE(static_cast<sc_type>(type));
	if (!sce) return 0;
	switch (which) {
		case 1: return sce->val1;
		case 2: return sce->val2;
		case 3: return sce->val3;
		case 4: return sce->val4;
	}
	return 0;
}

static int32_t api_status_sc_id(const char* name)
{
	if (!name || !*name) return SC_NONE;
	int64 v = 0;
	if (script_get_constant(name, &v))
		return static_cast<int32_t>(v);
	std::string prefixed = std::string("SC_") + name;
	if (script_get_constant(prefixed.c_str(), &v))
		return static_cast<int32_t>(v);
	return SC_NONE;
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

static bool api_atcmd_register(const char* name, int level,
                               plugin_atcmd_func func, void* user_data)
{
	return atcommand_plugin_register(name, level, func, user_data);
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

static bool api_npc_add_script_file(const char* path)
{
	if (!path || !*path) return false;
	// `false` = queue only; the file is parsed later by do_init_npc.
	return npc_addsrcfile(path, false) != 0;
}

static bool api_npc_del_script_file(const char* path)
{
	if (!path || !*path) return false;
	bool was_present =
		std::find(npc_src_files.begin(), npc_src_files.end(),
		          std::string(path)) != npc_src_files.end();
	npc_delsrcfile(path);
	return was_present;
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

// ---- NPC script dialog wrappers ----
static void api_clif_scriptmes(map_session_data* sd, uint32_t oid, const char* msg)
{
	if (sd && msg) clif_scriptmes(*sd, oid, msg);
}
static void api_clif_scriptnext(map_session_data* sd, uint32_t oid)
{
	if (sd) clif_scriptnext(*sd, oid);
}
static void api_clif_scriptclose(map_session_data* sd, uint32_t oid)
{
	if (sd) clif_scriptclose(*sd, oid);
}
static void api_clif_scriptmenu(map_session_data* sd, uint32_t oid, const char* menu)
{
	if (sd && menu) clif_scriptmenu(*sd, oid, menu);
}
static void api_clif_scriptinput(map_session_data* sd, uint32_t oid)
{
	if (sd) clif_scriptinput(*sd, oid);
}
static void api_clif_scriptinputstr(map_session_data* sd, uint32_t oid)
{
	if (sd) clif_scriptinputstr(*sd, oid);
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
// Packet API wrappers
// ============================================================

// Single trampoline shared by every plugin-registered packet. Looks up
// the {func, user_data} closure by cmd id from RFIFOW(fd, 0).
static void plugin_packet_trampoline(int32 fd, map_session_data* sd)
{
	uint16_t cmd = RFIFOW(fd, 0);
	auto it = plugin_packet_handlers.find(cmd);
	if (it == plugin_packet_handlers.end()) return;
	it->second.func(fd, sd, it->second.user_data);
}

static bool api_packet_register(uint16_t cmd, int16_t length,
                                plugin_packet_func func, void* user_data)
{
	if (!func || cmd < MIN_PACKET_DB || cmd > MAX_PACKET_DB)
		return false;
	// `packetdb_addpacket` is variadic with offset list terminated by 0.
	// Plugins compute their own offsets via read_b/w/l, so we pass none.
	// We install our trampoline; the real func+user_data lives in the map.
	packetdb_addpacket(cmd, static_cast<uint16>(length),
	                   plugin_packet_trampoline, 0);
	plugin_packet_handlers[cmd] = { func, user_data };
	return true;
}

static bool api_packet_unregister(uint16_t cmd)
{
	if (cmd < MIN_PACKET_DB || cmd > MAX_PACKET_DB)
		return false;
	packetdb_addpacket(cmd, 0, nullptr, 0);
	plugin_packet_handlers.erase(cmd);
	return true;
}

static uint8_t  api_packet_read_b(int32_t fd, int32_t off) { return RFIFOB(fd, off); }
static uint16_t api_packet_read_w(int32_t fd, int32_t off) { return RFIFOW(fd, off); }
static uint32_t api_packet_read_l(int32_t fd, int32_t off) { return RFIFOL(fd, off); }
static const char* api_packet_read_str(int32_t fd, int32_t off) { return RFIFOCP(fd, off); }
static int32_t  api_packet_read_rest(int32_t fd) { return static_cast<int32_t>(RFIFOREST(fd)); }

static void api_packet_send_self(int32_t fd, const void* data, int32_t len)
{
	if (!data || len <= 0 || !session_isActive(fd)) return;
	WFIFOHEAD(fd, len);
	std::memcpy(WFIFOP(fd, 0), data, len);
	WFIFOSET(fd, len);
}

static void api_packet_send_target(block_list* bl, const void* data,
                                   int32_t len, int32_t target)
{
	if (!data || len <= 0) return;
	clif_send(data, len, bl, static_cast<send_target>(target));
}

// ============================================================
// Plugin status change (SC) engine
// ============================================================

// Map an e_plugin_scb mask to the engine's e_scb_flag list and trigger
// a stat recalculation on `bl`.
static void plugin_sc_recalc(block_list* bl, int32_t calc_flag)
{
	if (!bl || !calc_flag) return;
	std::vector<e_scb_flag> flags;
	if (calc_flag & PLUGIN_SCB_STR)   flags.push_back(SCB_STR);
	if (calc_flag & PLUGIN_SCB_AGI)   flags.push_back(SCB_AGI);
	if (calc_flag & PLUGIN_SCB_VIT)   flags.push_back(SCB_VIT);
	if (calc_flag & PLUGIN_SCB_INT)   flags.push_back(SCB_INT);
	if (calc_flag & PLUGIN_SCB_DEX)   flags.push_back(SCB_DEX);
	if (calc_flag & PLUGIN_SCB_LUK)   flags.push_back(SCB_LUK);
	if (calc_flag & PLUGIN_SCB_MAXHP) flags.push_back(SCB_MAXHP);
	if (calc_flag & PLUGIN_SCB_MAXSP) flags.push_back(SCB_MAXSP);
	if (calc_flag & PLUGIN_SCB_SPEED) flags.push_back(SCB_SPEED);
	if (!flags.empty())
		status_calc_bl(bl, flags);
}

// Locate an active plugin SC entry; returns nullptr if not present.
static PluginSCActive* plugin_sc_find(int32_t bl_id, int32_t sc_id)
{
	auto it = plugin_sc_active.find(bl_id);
	if (it == plugin_sc_active.end()) return nullptr;
	for (auto& e : it->second)
		if (e.sc_id == sc_id) return &e;
	return nullptr;
}

// Show/hide the client status icon for an SC, if it has one.
static void plugin_sc_icon(block_list* bl, int32_t sc_id, bool show, int64_t duration_ms,
                           int32_t v1, int32_t v2, int32_t v3)
{
	if (sc_id < 0 || sc_id >= static_cast<int>(plugin_sc_defs.size())) return;
	int32_t icon = plugin_sc_defs[sc_id].icon;
	if (icon <= 0) return;
	clif_status_change(bl, icon, show ? 1 : 0,
	                   show ? static_cast<t_tick>(duration_ms) : 0, v1, v2, v3);
}

static int32 plugin_sc_expire_timer(int32 /*tid*/, t_tick /*tick*/, int32 id, intptr_t data)
{
	int32_t sc_id = static_cast<int32_t>(data);
	auto it = plugin_sc_active.find(id);
	if (it == plugin_sc_active.end()) return 0;

	int32_t calc_flag = 0;
	bool removed = false;
	for (auto e = it->second.begin(); e != it->second.end(); ++e) {
		if (e->sc_id == sc_id) {
			if (sc_id >= 0 && sc_id < static_cast<int>(plugin_sc_defs.size()))
				calc_flag = plugin_sc_defs[sc_id].calc_flag;
			it->second.erase(e);
			removed = true;
			break;
		}
	}
	if (it->second.empty())
		plugin_sc_active.erase(it);

	if (removed) {
		block_list* bl = map_id2bl(id);
		if (bl) {
			plugin_sc_icon(bl, sc_id, false, 0, 0, 0, 0);
			plugin_sc_recalc(bl, calc_flag);
		}
	}
	return 0;
}

static int32_t api_sc_register(const char* name, int32_t calc_flag, int32_t icon,
                               plugin_sc_calc_func calc, void* user_data)
{
	if (!calc) return -1;
	int32_t id = static_cast<int32_t>(plugin_sc_defs.size());
	plugin_sc_defs.push_back({ name ? name : "", calc_flag, icon, calc, user_data });
	return id;
}

static bool api_sc_start(block_list* bl, int32_t sc_id,
                         int32_t v1, int32_t v2, int32_t v3, int32_t v4,
                         int64_t duration_ms)
{
	if (!bl || sc_id < 0 || sc_id >= static_cast<int>(plugin_sc_defs.size()))
		return false;

	auto& vec = plugin_sc_active[bl->id];
	PluginSCActive* e = nullptr;
	for (auto& a : vec)
		if (a.sc_id == sc_id) { e = &a; break; }
	if (!e) {
		vec.push_back({ sc_id, 0, 0, 0, 0, INVALID_TIMER });
		e = &vec.back();
	}

	// Replace stored params; reset the expiry timer.
	if (e->timer != INVALID_TIMER) {
		delete_timer(e->timer, plugin_sc_expire_timer);
		e->timer = INVALID_TIMER;
	}
	e->val1 = v1; e->val2 = v2; e->val3 = v3; e->val4 = v4;
	if (duration_ms > 0)
		e->timer = add_timer(gettick() + static_cast<t_tick>(duration_ms),
		                     plugin_sc_expire_timer, bl->id, static_cast<intptr_t>(sc_id));

	plugin_sc_icon(bl, sc_id, true, duration_ms, v1, v2, v3);
	plugin_sc_recalc(bl, plugin_sc_defs[sc_id].calc_flag);
	return true;
}

static bool api_sc_end(block_list* bl, int32_t sc_id)
{
	if (!bl) return false;
	auto it = plugin_sc_active.find(bl->id);
	if (it == plugin_sc_active.end()) return false;

	for (auto e = it->second.begin(); e != it->second.end(); ++e) {
		if (e->sc_id != sc_id) continue;
		if (e->timer != INVALID_TIMER)
			delete_timer(e->timer, plugin_sc_expire_timer);
		it->second.erase(e);
		if (it->second.empty())
			plugin_sc_active.erase(it);
		plugin_sc_icon(bl, sc_id, false, 0, 0, 0, 0);
		if (sc_id >= 0 && sc_id < static_cast<int>(plugin_sc_defs.size()))
			plugin_sc_recalc(bl, plugin_sc_defs[sc_id].calc_flag);
		return true;
	}
	return false;
}

static bool api_sc_active(block_list* bl, int32_t sc_id,
                          int32_t* o1, int32_t* o2, int32_t* o3, int32_t* o4)
{
	if (!bl) return false;
	PluginSCActive* e = plugin_sc_find(bl->id, sc_id);
	if (!e) return false;
	if (o1) *o1 = e->val1;
	if (o2) *o2 = e->val2;
	if (o3) *o3 = e->val3;
	if (o4) *o4 = e->val4;
	return true;
}

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
		api_script_suspend,
		api_script_resume,
		api_script_set_var_num,
		api_script_set_var_str,
		api_script_get_var_num,
		api_script_get_var_str,
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
		api_pc_bonus,
		api_pc_bonus2,
		api_pc_bonus3,
		api_pc_bonus4,
		api_pc_bonus5,
		api_pc_countitem,
		api_pc_read_param,
		api_pc_get_equip_nameid,
		api_pc_get_npc_id,
		api_pc_get_npc_menu,
		api_pc_get_npc_amount,
		api_pc_get_npc_str,
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
		api_status_change_start,
		api_status_change_end,
		api_status_change_clear,
		api_status_has_change,
		api_status_change_val,
		api_status_sc_id,
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
		api_npc_add_script_file,
		api_npc_del_script_file,
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
		api_clif_scriptmes,
		api_clif_scriptnext,
		api_clif_scriptclose,
		api_clif_scriptmenu,
		api_clif_scriptinput,
		api_clif_scriptinputstr,
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

	// packet sub-struct
	{
		api_packet_register,
		api_packet_unregister,
		api_packet_read_b,
		api_packet_read_w,
		api_packet_read_l,
		api_packet_read_str,
		api_packet_read_rest,
		api_packet_send_self,
		api_packet_send_target,
	},

	// sc sub-struct
	{
		api_sc_register,
		api_sc_start,
		api_sc_end,
		api_sc_active,
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
// Public: notify that a script_state is being freed
// ============================================================

void plugin_script_state_freed(script_state* st)
{
	if (st) suspended_states.erase(st);
}

// ============================================================
// Public: plugin SC integration points
// ============================================================

int32_t plugin_status_calc(block_list* bl, int32_t scb_kind, int32_t cur_value)
{
	if (!bl || plugin_sc_active.empty()) return cur_value;
	auto it = plugin_sc_active.find(bl->id);
	if (it == plugin_sc_active.end()) return cur_value;

	int32_t v = cur_value;
	for (const auto& a : it->second) {
		if (a.sc_id < 0 || a.sc_id >= static_cast<int>(plugin_sc_defs.size()))
			continue;
		const PluginSCDef& def = plugin_sc_defs[a.sc_id];
		if (!(def.calc_flag & scb_kind) || !def.calc)
			continue;
		v = def.calc(bl, a.sc_id, scb_kind, v,
		             a.val1, a.val2, a.val3, a.val4, def.user_data);
	}
	return v;
}

void plugin_sc_clear(block_list* bl)
{
	if (!bl) return;
	auto it = plugin_sc_active.find(bl->id);
	if (it == plugin_sc_active.end()) return;
	for (auto& a : it->second)
		if (a.timer != INVALID_TIMER)
			delete_timer(a.timer, plugin_sc_expire_timer);
	plugin_sc_active.erase(it);
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

	// Clear plugin-registered client packets for the same reason: clif_parse
	// must not dispatch into a function whose DLL is about to vanish.
	for (auto& kv : plugin_packet_handlers)
		packetdb_addpacket(kv.first, 0, nullptr, 0);
	plugin_packet_handlers.clear();

	// Cancel and drop all active plugin SC timers/instances.
	for (auto& kv : plugin_sc_active)
		for (auto& a : kv.second)
			if (a.timer != INVALID_TIMER)
				delete_timer(a.timer, plugin_sc_expire_timer);
	plugin_sc_active.clear();
	plugin_sc_defs.clear();

	for (auto it = loaded_plugins.rbegin(); it != loaded_plugins.rend(); ++it) {
		if (it->pfn_final)
			it->pfn_final();
		close_plugin(it->handle);
	}
	loaded_plugins.clear();
	plugin_cmds.clear();
	suspended_states.clear();
	for (int i = 0; i < HOOK_MAX; ++i)
		hook_chains[i].clear();
}
