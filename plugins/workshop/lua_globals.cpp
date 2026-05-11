/*
 * Lua global wrappers around the rAthena plugin API.
 *
 * Every wrapper here is exposed to Lua as a top-level global, so mod authors
 * write `announce("hi")` instead of `ra.announce("hi")`.
 *
 * Naming follows rAthena script command conventions where there is overlap
 * (announce, getitem, warp, gettimetick) and otherwise uses snake_case.
 */

#include "lua_bridge.hpp"
#include "db_store.hpp"
#include "workshop.hpp"

#include <yaml-cpp/yaml.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
// MSVC has localtime_s(tm*, time_t*) — swap args to match POSIX localtime_r
static inline void localtime_r(const time_t* t, struct tm* out) { localtime_s(out, t); }
#endif

namespace {

// ---- registry of Lua callbacks bound to plugin hooks / commands ----
//
// plugin_api passes a `user_data` closure pointer back to every script
// command / atcommand / hook callback, so we allocate one record per
// registration and hand its address to the engine. On reload the same
// record is reused — only its Lua ref is updated to point at the freshly
// loaded function — so the engine's command tables stay valid across
// Lua state recreations.

struct LuaScriptCmd {
    int         ref;       // luaL_ref into LUA_REGISTRYINDEX
    std::string name;
    std::string spec;
};

struct LuaAtcmd {
    int         ref;
    std::string name;
};

static std::map<std::string, LuaScriptCmd*> g_buildin_by_name;
static std::map<std::string, LuaAtcmd*>     g_atcmd_by_name;

// Hooks: one entry per (hook_type, ref). Stored separately so we can clean up.
struct HookCallback {
    int hook_type;
    int ref;
};
static std::vector<HookCallback> g_hook_callbacks;

// Coroutine state for the currently-running register_buildin handler.
// Defined here (rather than next to drive_coro further down) so that
// var-storage and dialog wrappers can reach `g_active_coro` directly.
struct BuildinCoro {
    script_state* st             = nullptr;
    lua_State*    T              = nullptr;
    int           thread_ref     = LUA_NOREF; // keeps T alive against GC
    void*         token          = nullptr;   // null until first suspend
    bool          engine_resumed = false;     // dialog flow: engine drives resumption
    std::string   name;
};

static thread_local BuildinCoro* g_active_coro = nullptr;

// ---- helpers ----

static lua_State* L_get() { return workshop::LuaBridge::instance().L(); }

static void push_player(lua_State* L, map_session_data* sd) {
    if (!sd) {
        lua_pushnil(L);
        return;
    }
    lua_newtable(L);
    lua_pushinteger(L, g_api->pc.get_aid(sd));
    lua_setfield(L, -2, "aid");
    lua_pushstring(L, g_api->pc.get_name(sd));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, g_api->pc.get_blv(sd));
    lua_setfield(L, -2, "base_level");
    lua_pushinteger(L, g_api->pc.get_jlv(sd));
    lua_setfield(L, -2, "job_level");
    int16_t m = g_api->pc.get_mapid(sd);
    const char* mn = g_api->map.id2name(m);
    lua_pushstring(L, mn ? mn : "");
    lua_setfield(L, -2, "map");
    lua_pushinteger(L, g_api->pc.get_pos_x(sd));
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, g_api->pc.get_pos_y(sd));
    lua_setfield(L, -2, "y");
}

static map_session_data* sd_from_aid(int32_t aid) {
    return g_api->map.id2sd(aid);
}

// Resolve a player from the first Lua argument: accepts either an account_id
// integer or a player table that has an `aid` field.
static map_session_data* sd_from_arg(lua_State* L, int idx) {
    if (lua_isinteger(L, idx) || lua_isnumber(L, idx)) {
        return sd_from_aid((int32_t)lua_tointeger(L, idx));
    }
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "aid");
        int32_t aid = (int32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return sd_from_aid(aid);
    }
    if (lua_isstring(L, idx)) {
        return g_api->map.nick2sd(lua_tostring(L, idx), false);
    }
    return nullptr;
}

// ---- wrappers ----

// announce("text" [, color])
//   color is 0 (yellow / BC_DEFAULT) or 0x10 (blue), default yellow.
static int lw_announce(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    int color = (int)luaL_optinteger(L, 2, 0);
    g_api->clif.broadcast(nullptr, msg, color, 0); // 0 = ALL_CLIENT
    return 0;
}

// log_info("text"), log_warn, log_error
#define LW_LOG(fname, api_fn)                              \
    static int fname(lua_State* L) {                       \
        const char* m = luaL_checkstring(L, 1);            \
        char buf[1024];                                    \
        snprintf(buf, sizeof(buf), "%s", m);               \
        g_api->log.api_fn(buf);                            \
        return 0;                                          \
    }
LW_LOG(lw_log_info,    info)
LW_LOG(lw_log_status,  status)
LW_LOG(lw_log_warning, warning)
LW_LOG(lw_log_error,   error)
#undef LW_LOG

// get_player(aid) -> player table or nil
static int lw_get_player(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    push_player(L, sd);
    return 1;
}

// get_player_by_name("nick") -> player table or nil
static int lw_get_player_by_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    map_session_data* sd = g_api->map.nick2sd(name, false);
    push_player(L, sd);
    return 1;
}

// getitem(player, item_id [, amount]) -> 1 on success, 0 on failure
static int lw_getitem(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }

    plugin_item_t it{};
    it.nameid = (uint32_t)luaL_checkinteger(L, 2);
    it.identify = 1;
    int32_t amount = (int32_t)luaL_optinteger(L, 3, 1);

    int rc = g_api->pc.additem(sd, &it, amount, 7); // LOG_TYPE_SCRIPT
    lua_pushinteger(L, rc == 0 ? 1 : 0);
    return 1;
}

// gainexp(player, base_exp [, job_exp])
static int lw_gainexp(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    uint64_t base = (uint64_t)luaL_checkinteger(L, 2);
    uint64_t job  = (uint64_t)luaL_optinteger(L, 3, 0);
    g_api->pc.gainexp(sd, nullptr, base, job, 0);
    lua_pushinteger(L, 1);
    return 1;
}

// warp(player, "mapname", x, y) -> 1 on success
static int lw_warp(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    const char* m = luaL_checkstring(L, 2);
    int32_t x = (int32_t)luaL_checkinteger(L, 3);
    int32_t y = (int32_t)luaL_checkinteger(L, 4);
    uint16_t mid = g_api->map.name2id(m);
    int rc = g_api->pc.setpos(sd, mid, x, y, 0);
    lua_pushinteger(L, rc == 0 ? 1 : 0);
    return 1;
}

// heal(player, hp [, sp])
static int lw_heal(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int64_t hp = luaL_checkinteger(L, 2);
    int64_t sp = luaL_optinteger(L, 3, 0);
    g_api->status.heal(g_api->pc.as_bl(sd), hp, sp, 0);
    lua_pushinteger(L, 1);
    return 1;
}

// ---- status changes (SC_*) ----
//
// Two kinds of SC are reachable from Lua:
//   * engine built-ins  — SC_FREEZE etc., applied via g_api->status.change_*
//   * plugin-defined SC — registered with register_sc(), applied via the
//     g_api->sc.* table (own calc callback, own side table, timer-driven).
// register_sc() returns an id that lands in g_plugin_sc_ids; the sc_*
// wrappers below route on membership so callers use one set of functions
// for both.

static std::set<int32_t> g_plugin_sc_ids;

static bool is_plugin_sc(int32_t id) {
    return g_plugin_sc_ids.find(id) != g_plugin_sc_ids.end();
}

// Resolve a Lua arg that's either a numeric sc_type / plugin-SC id or an
// SC name string. Returns SC_NONE (-1) for an unrecognised string.
static int32_t sc_type_from_arg(lua_State* L, int idx) {
    if (lua_isinteger(L, idx) || lua_isnumber(L, idx))
        return (int32_t)lua_tointeger(L, idx);
    if (lua_isstring(L, idx))
        return g_api->status.sc_id(lua_tostring(L, idx));
    return -1; // SC_NONE
}

// sc_id("FREEZE") -> numeric sc_type (accepts "SC_FREEZE" too); -1 if unknown
static int lw_sc_id(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, g_api->status.sc_id(name));
    return 1;
}

// ---- plugin-defined SC: register_sc + the calc trampoline ----

// PLUGIN_SCB_* — which stats a plugin SC influences. Mirrors e_plugin_scb.
static int32_t scb_flag_from_arg(lua_State* L, int idx) {
    if (lua_isinteger(L, idx) || lua_isnumber(L, idx))
        return (int32_t)lua_tointeger(L, idx);
    int32_t flag = 0;
    if (lua_istable(L, idx)) {
        int n = (int)lua_rawlen(L, idx);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            const char* s = lua_tostring(L, -1);
            if (s) {
                std::string up;
                for (const char* p = s; *p; ++p)
                    up += (char)std::toupper((unsigned char)*p);
                if      (up == "STR")   flag |= PLUGIN_SCB_STR;
                else if (up == "AGI")   flag |= PLUGIN_SCB_AGI;
                else if (up == "VIT")   flag |= PLUGIN_SCB_VIT;
                else if (up == "INT")   flag |= PLUGIN_SCB_INT;
                else if (up == "DEX")   flag |= PLUGIN_SCB_DEX;
                else if (up == "LUK")   flag |= PLUGIN_SCB_LUK;
                else if (up == "MAXHP") flag |= PLUGIN_SCB_MAXHP;
                else if (up == "MAXSP") flag |= PLUGIN_SCB_MAXSP;
                else if (up == "SPEED") flag |= PLUGIN_SCB_SPEED;
            }
            lua_pop(L, 1);
        }
    }
    return flag;
}

// Engine -> Lua bridge for a registered plugin SC's calc callback. The
// luaL_ref of the Lua function is carried as `user_data`. Invoked once
// per affected stat during status recalculation; the Lua side returns
// the new value for `cur_value`.
static int32_t plugin_sc_calc_trampoline(block_list* bl, int32_t sc_id,
                                         int32_t scb_kind, int32_t cur_value,
                                         int32_t v1, int32_t v2,
                                         int32_t v3, int32_t v4,
                                         void* user_data) {
    int ref = (int)(intptr_t)user_data;
    lua_State* L = L_get();
    if (!L) return cur_value;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return cur_value; }

    lua_newtable(L);
    lua_pushinteger(L, sc_id);     lua_setfield(L, -2, "sc_id");
    lua_pushinteger(L, scb_kind);  lua_setfield(L, -2, "stat");  // a PLUGIN_SCB_* bit
    lua_pushinteger(L, cur_value); lua_setfield(L, -2, "cur");
    lua_pushinteger(L, v1);        lua_setfield(L, -2, "val1");
    lua_pushinteger(L, v2);        lua_setfield(L, -2, "val2");
    lua_pushinteger(L, v3);        lua_setfield(L, -2, "val3");
    lua_pushinteger(L, v4);        lua_setfield(L, -2, "val4");
    if (g_api->bl.get_type(bl) == PLUGIN_BL_PC) {
        if (map_session_data* sd = g_api->bl.as_sd(bl)) {
            push_player(L, sd);
            lua_setfield(L, -2, "player");
        }
    }

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        wlog_warning("register_sc calc error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return cur_value;
    }
    int32_t result = (lua_isinteger(L, -1) || lua_isnumber(L, -1))
        ? (int32_t)lua_tointeger(L, -1) : cur_value;
    lua_pop(L, 1);
    return result;
}

// register_sc("name", calc_flag, fn [, icon]) -> sc id (>=0) or -1
//   calc_flag — an int bitmask OR a table of stat names:
//               {'STR','AGI','VIT','INT','DEX','LUK','MAXHP','MAXSP','SPEED'}
//   fn(ctx)   — called per affected stat; ctx = {player?, sc_id, stat,
//               cur, val1..val4}; return the new value for ctx.cur.
//   icon      — optional EFST_* status-bar icon (0 = none).
static int lw_register_sc(lua_State* L) {
    const char* name  = luaL_checkstring(L, 1);
    int32_t calc_flag = scb_flag_from_arg(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    int32_t icon = (int32_t)luaL_optinteger(L, 4, 0);

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int32_t id = g_api->sc.register_sc(name, calc_flag, icon,
                                       plugin_sc_calc_trampoline,
                                       (void*)(intptr_t)ref);
    if (id >= 0) {
        g_plugin_sc_ids.insert(id);
    } else {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    lua_pushinteger(L, id);
    return 1;
}

// sc_start(player, type, duration_ms [, val1, val2, val3, val4 [, flag]]) -> bool
//   type is a number, an SC name, or a register_sc() id. For engine SCs
//   the rate is always 100% from Lua and `flag` is a SCSTART_* bitmask;
//   for plugin SCs `flag` is ignored and duration <= 0 means permanent.
static int lw_sc_start(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushboolean(L, 0); return 1; }
    int32_t type = sc_type_from_arg(L, 2);
    int64_t dur  = luaL_checkinteger(L, 3);
    int32_t v1   = (int32_t)luaL_optinteger(L, 4, 0);
    int32_t v2   = (int32_t)luaL_optinteger(L, 5, 0);
    int32_t v3   = (int32_t)luaL_optinteger(L, 6, 0);
    int32_t v4   = (int32_t)luaL_optinteger(L, 7, 0);
    bool ok;
    if (is_plugin_sc(type)) {
        ok = g_api->sc.start(g_api->pc.as_bl(sd), type, v1, v2, v3, v4, dur);
    } else {
        int32_t flag = (int32_t)luaL_optinteger(L, 8, 0);
        ok = g_api->status.change_start(nullptr, g_api->pc.as_bl(sd),
                                        type, 10000, v1, v2, v3, v4,
                                        dur, flag);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// sc_start_from(src_player, target_player, type, duration_ms [, v1..v4 [, flag]])
//   like sc_start but attributes an engine SC to a source. Plugin SCs
//   have no source channel, so this falls back to a plain start for them.
static int lw_sc_start_from(lua_State* L) {
    map_session_data* src = lua_isnoneornil(L, 1) ? nullptr : sd_from_arg(L, 1);
    map_session_data* tgt = sd_from_arg(L, 2);
    if (!tgt) { lua_pushboolean(L, 0); return 1; }
    int32_t type = sc_type_from_arg(L, 3);
    int64_t dur  = luaL_checkinteger(L, 4);
    int32_t v1   = (int32_t)luaL_optinteger(L, 5, 0);
    int32_t v2   = (int32_t)luaL_optinteger(L, 6, 0);
    int32_t v3   = (int32_t)luaL_optinteger(L, 7, 0);
    int32_t v4   = (int32_t)luaL_optinteger(L, 8, 0);
    bool ok;
    if (is_plugin_sc(type)) {
        ok = g_api->sc.start(g_api->pc.as_bl(tgt), type, v1, v2, v3, v4, dur);
    } else {
        int32_t flag = (int32_t)luaL_optinteger(L, 9, 0);
        ok = g_api->status.change_start(src ? g_api->pc.as_bl(src) : nullptr,
                                        g_api->pc.as_bl(tgt),
                                        type, 10000, v1, v2, v3, v4,
                                        dur, flag);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// sc_end(player, type) -> 1 if a status was removed
static int lw_sc_end(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t type = sc_type_from_arg(L, 2);
    if (is_plugin_sc(type)) {
        lua_pushinteger(L, g_api->sc.end(g_api->pc.as_bl(sd), type) ? 1 : 0);
    } else {
        lua_pushinteger(L, g_api->status.change_end(g_api->pc.as_bl(sd), type));
    }
    return 1;
}

// sc_clear(player [, all]) — all=true wipes even permanent engine statuses.
// (Plugin SCs are cleared per-id with sc_end; there's no bulk clear.)
static int lw_sc_clear(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    bool all = lua_toboolean(L, 2);
    g_api->status.change_clear(g_api->pc.as_bl(sd), all ? 0 : 1);
    return 0;
}

// sc_active(player, type) -> bool
static int lw_sc_active(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushboolean(L, 0); return 1; }
    int32_t type = sc_type_from_arg(L, 2);
    bool active = is_plugin_sc(type)
        ? g_api->sc.active(g_api->pc.as_bl(sd), type, nullptr, nullptr, nullptr, nullptr)
        : g_api->status.has_change(g_api->pc.as_bl(sd), type);
    lua_pushboolean(L, active ? 1 : 0);
    return 1;
}

// sc_val(player, type, which) -> int  (which is 1..4)
static int lw_sc_val(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t type  = sc_type_from_arg(L, 2);
    int32_t which = (int32_t)luaL_checkinteger(L, 3);
    if (is_plugin_sc(type)) {
        int32_t v[4] = {0, 0, 0, 0};
        if (which >= 1 && which <= 4 &&
            g_api->sc.active(g_api->pc.as_bl(sd), type, &v[0], &v[1], &v[2], &v[3])) {
            lua_pushinteger(L, v[which - 1]);
        } else {
            lua_pushinteger(L, 0);
        }
    } else {
        lua_pushinteger(L, g_api->status.change_val(g_api->pc.as_bl(sd), type, which));
    }
    return 1;
}

// monster("mapname", x, y, "name", mob_id [, amount])
static int lw_monster(lua_State* L) {
    const char* m  = luaL_checkstring(L, 1);
    int x          = (int)luaL_checkinteger(L, 2);
    int y          = (int)luaL_checkinteger(L, 3);
    const char* mn = luaL_optstring(L, 4, "");
    int mob_id     = (int)luaL_checkinteger(L, 5);
    int amt        = (int)luaL_optinteger(L, 6, 1);
    uint16_t mid   = g_api->map.name2id(m);
    int spawned = g_api->mob.once_spawn(nullptr, (int16_t)mid, (int16_t)x, (int16_t)y,
                                         mn, mob_id, amt, "", 0, 0);
    lua_pushinteger(L, spawned);
    return 1;
}

// message(player, "text") — chat-window message
static int lw_message(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    const char* m = luaL_checkstring(L, 2);
    g_api->pc.message(g_api->pc.get_fd(sd), m);
    return 0;
}

// emotion(player, emote_id)
static int lw_emotion(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t e = (int32_t)luaL_checkinteger(L, 2);
    g_api->clif.emotion(g_api->pc.as_bl(sd), e);
    return 0;
}

// specialeffect(player, effect_id [, target])
//   target: 0=ALL_CLIENT 1=ALL_SAMEMAP 2=AREA 3=AREA_WOS 24=SELF
static int lw_specialeffect(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t eff = (int32_t)luaL_checkinteger(L, 2);
    int32_t tgt = (int32_t)luaL_optinteger(L, 3, 2); // AREA
    g_api->clif.specialeffect(g_api->pc.as_bl(sd), eff, tgt);
    return 0;
}

// item_name(item_id) -> display name
static int lw_item_name(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    const char* n = g_api->item_api.db_get_ename(id);
    lua_pushstring(L, n ? n : "");
    return 1;
}

// skill_name(skill_id) -> AEGIS name
static int lw_skill_name(lua_State* L) {
    uint16_t id = (uint16_t)luaL_checkinteger(L, 1);
    const char* n = g_api->skill.get_name(id);
    lua_pushstring(L, n ? n : "");
    return 1;
}

// gettick() -> milliseconds since server start (matches rAthena gettick)
static int lw_gettick(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_api->timer.gettick());
    return 1;
}

// ---- inventory ----

// delitem(player, slot, amount [, type, reason]) -> 1 on success
//   slot is the inventory index (0..MAX_INVENTORY-1).
//   type: 0=normal, 1=fail-allowed; reason: log reason code (LOG_TYPE_SCRIPT=7).
static int lw_delitem(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t  n      = (int32_t)luaL_checkinteger(L, 2);
    int32_t  amount = (int32_t)luaL_checkinteger(L, 3);
    int32_t  type   = (int32_t)luaL_optinteger(L, 4, 0);
    int16_t  reason = (int16_t)luaL_optinteger(L, 5, 0);
    char rc = g_api->pc.delitem(sd, n, amount, type, reason, 7);
    lua_pushinteger(L, rc == 0 ? 1 : 0);
    return 1;
}

// item_exists(item_id) -> bool
static int lw_item_exists(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, g_api->item_api.db_exists(id) ? 1 : 0);
    return 1;
}

// item_internal_name(item_id) -> "Apple" (the AEGIS / db key)
static int lw_item_internal_name(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    const char* n = g_api->item_api.db_get_name(id);
    lua_pushstring(L, n ? n : "");
    return 1;
}

// item_type(item_id) -> int (IT_HEALING=0, IT_USABLE=2, IT_ETC=3, ...)
static int lw_item_type(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, g_api->item_api.db_get_type(id));
    return 1;
}

// ---- money ----

// payzeny(player, amount) -> 1 on success
static int lw_payzeny(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t z = (int32_t)luaL_checkinteger(L, 2);
    char rc = g_api->pc.payzeny(sd, z, 7);  // LOG_TYPE_SCRIPT
    lua_pushinteger(L, rc == 0 ? 1 : 0);
    return 1;
}

// give_zeny(player, amount) -> 1 on success
static int lw_give_zeny(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t z = (int32_t)luaL_checkinteger(L, 2);
    char rc = g_api->pc.getzeny(sd, z, 7);
    lua_pushinteger(L, rc == 0 ? 1 : 0);
    return 1;
}

// ---- combat / skills ----

// damage(src, target, hp [, sp, walkdelay, flag, skill_id]) -> remaining hp
//   src may be nil/0 (no source). flag: 1=no death, 2=no aggro.
static int lw_damage(lua_State* L) {
    map_session_data* src_sd = lua_isnoneornil(L, 1) ? nullptr : sd_from_arg(L, 1);
    map_session_data* tgt_sd = sd_from_arg(L, 2);
    if (!tgt_sd) { lua_pushinteger(L, 0); return 1; }
    int64_t  hp        = luaL_checkinteger(L, 3);
    int64_t  sp        = luaL_optinteger(L, 4, 0);
    int64_t  walkdelay = luaL_optinteger(L, 5, 0);
    int32_t  flag      = (int32_t)luaL_optinteger(L, 6, 0);
    uint16_t skill_id  = (uint16_t)luaL_optinteger(L, 7, 0);

    block_list* src_bl = src_sd ? g_api->pc.as_bl(src_sd) : nullptr;
    int32_t r = g_api->status.damage(src_bl, g_api->pc.as_bl(tgt_sd),
                                      hp, sp, walkdelay, flag, skill_id);
    lua_pushinteger(L, r);
    return 1;
}

// use_skill(player, skill_id, level [, target_aid]) -> 1 on success
//   target_aid defaults to the player itself.
static int lw_use_skill(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    uint16_t id  = (uint16_t)luaL_checkinteger(L, 2);
    uint16_t lvl = (uint16_t)luaL_checkinteger(L, 3);
    int32_t  tgt = (int32_t)luaL_optinteger(L, 4, g_api->pc.get_aid(sd));
    int32_t r = g_api->skill.use_id(sd, id, lvl, tgt);
    lua_pushinteger(L, r);
    return 1;
}

// get_skill_lv(player, skill_id) -> learned level
static int lw_get_skill_lv(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    uint16_t id = (uint16_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, g_api->skill.get_lv(sd, id));
    return 1;
}

// skill_id("MG_FIREBOLT") -> numeric id, 0 if unknown
static int lw_skill_id(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, g_api->skill.name2id(name));
    return 1;
}

// skill_inf(skill_id) -> INF flags (1=ATTACK, 2=GROUND, 4=SELF, ...)
static int lw_skill_inf(lua_State* L) {
    uint16_t id = (uint16_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, g_api->skill.get_inf(id));
    return 1;
}

// ---- battle config (conf/battle/*.conf) ----

// battle_get("base_exp_rate") -> int (0 if the name is unknown — use
// battle_has to disambiguate from a setting that is genuinely 0)
static int lw_battle_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, g_api->battle.get(name));
    return 1;
}

// battle_set("base_exp_rate", value) -> bool
//   value may be a number or a boolean (true -> 1, false -> 0). The engine
//   clamps to the setting's declared [min, max]. Not persisted to .conf.
static int lw_battle_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int32_t value;
    if (lua_isboolean(L, 2))
        value = lua_toboolean(L, 2) ? 1 : 0;
    else
        value = (int32_t)luaL_checkinteger(L, 2);
    lua_pushboolean(L, g_api->battle.set(name, value) ? 1 : 0);
    return 1;
}

// battle_has("base_exp_rate") -> bool
static int lw_battle_has(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, g_api->battle.has(name) ? 1 : 0);
    return 1;
}

// ---- map ----

// mapindex("prontera") -> uint16
static int lw_mapindex(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    lua_pushinteger(L, g_api->map.name2id(m));
    return 1;
}

// mapname(idx) -> "prontera"
static int lw_mapname(lua_State* L) {
    uint16_t idx = (uint16_t)luaL_checkinteger(L, 1);
    const char* n = g_api->map.id2name(idx);
    lua_pushstring(L, n ? n : "");
    return 1;
}

// mapflag("prontera", flag_id) -> int
static int lw_mapflag(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    int32_t flag  = (int32_t)luaL_checkinteger(L, 2);
    int16_t mid   = (int16_t)g_api->map.name2id(m);
    lua_pushinteger(L, g_api->map.get_mapflag(mid, flag));
    return 1;
}

// ---- map iteration (for_each_*) ----

namespace iter {
struct Ctx {
    lua_State* L;
    int        ref;
    int32_t    matched;
};

static int32_t cb_player(block_list* bl, void* user) {
    auto* c = static_cast<Ctx*>(user);
    map_session_data* sd = g_api->bl.as_sd(bl);
    if (!sd) return 0;
    lua_rawgeti(c->L, LUA_REGISTRYINDEX, c->ref);
    if (!lua_isfunction(c->L, -1)) { lua_pop(c->L, 1); return 0; }
    push_player(c->L, sd);
    if (lua_pcall(c->L, 1, 1, 0) != LUA_OK) {
        wlog_warning("for_each error: %s", lua_tostring(c->L, -1));
        lua_pop(c->L, 1);
        return 0;
    }
    int matched = lua_toboolean(c->L, -1) ? 1 : 0;
    lua_pop(c->L, 1);
    if (matched) ++c->matched;
    return matched;
}

static int32_t cb_count_only(block_list* /*bl*/, void* user) {
    ++*static_cast<int32_t*>(user);
    return 1;
}
} // namespace iter

// for_each_player_in_map("mapname", function(player) -> bool|nil end) -> count
//   The callback's truthy return marks the player as "matched" (counted in
//   the return value); falsy return is fine too — used as filter / side
//   effect. Mirrors the rAthena getmapusers / mapwarp patterns.
static int lw_for_each_player_in_map(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int16_t mid = (int16_t)g_api->map.name2id(m);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    iter::Ctx ctx{L, ref, 0};
    g_api->map.foreachinmap(iter::cb_player, &ctx, mid, 1 << PLUGIN_BL_PC);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);

    lua_pushinteger(L, ctx.matched);
    return 1;
}

// for_each_player_in_area("map", x0, y0, x1, y1, fn) -> count
static int lw_for_each_player_in_area(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    int16_t x0 = (int16_t)luaL_checkinteger(L, 2);
    int16_t y0 = (int16_t)luaL_checkinteger(L, 3);
    int16_t x1 = (int16_t)luaL_checkinteger(L, 4);
    int16_t y1 = (int16_t)luaL_checkinteger(L, 5);
    luaL_checktype(L, 6, LUA_TFUNCTION);
    int16_t mid = (int16_t)g_api->map.name2id(m);

    lua_pushvalue(L, 6);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    iter::Ctx ctx{L, ref, 0};
    g_api->map.foreachinarea(iter::cb_player, &ctx, mid, x0, y0, x1, y1,
                              1 << PLUGIN_BL_PC);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, ctx.matched);
    return 1;
}

// count_players_in_map("mapname") -> int (cheap variant of for_each)
static int lw_count_players_in_map(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    int16_t mid = (int16_t)g_api->map.name2id(m);
    int32_t n = 0;
    g_api->map.foreachinmap(iter::cb_count_only, &n, mid, 1 << PLUGIN_BL_PC);
    lua_pushinteger(L, n);
    return 1;
}

// count_mobs_in_map("mapname") -> int
static int lw_count_mobs_in_map(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    int16_t mid = (int16_t)g_api->map.name2id(m);
    int32_t n = 0;
    g_api->map.foreachinmap(iter::cb_count_only, &n, mid, 1 << PLUGIN_BL_MOB);
    lua_pushinteger(L, n);
    return 1;
}

// ---- storage ----

// open_storage(player) -> 1 on success
static int lw_open_storage(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, g_api->storage.open(sd) == 0 ? 1 : 0);
    return 1;
}

// ---- quest ----

// quest_add(player, quest_id) -> 1 on success
static int lw_quest_add(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t qid = (int32_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, g_api->quest.add(sd, qid) == 0 ? 1 : 0);
    return 1;
}

// quest_status(player, quest_id, status)
//   status: 0=Q_INACTIVE, 1=Q_ACTIVE, 2=Q_COMPLETE
static int lw_quest_status(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t qid = (int32_t)luaL_checkinteger(L, 2);
    int     s   = (int)luaL_checkinteger(L, 3);
    lua_pushinteger(L, g_api->quest.update_status(sd, qid, s) == 0 ? 1 : 0);
    return 1;
}

// quest_check(player, quest_id [, type]) -> int
//   type: 0=HAVEQUEST, 1=PLAYTIME, 2=HUNTING
static int lw_quest_check(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, -1); return 1; }
    int32_t qid = (int32_t)luaL_checkinteger(L, 2);
    int     t   = (int)luaL_optinteger(L, 3, 0);
    lua_pushinteger(L, g_api->quest.check(sd, qid, t));
    return 1;
}

// ---- npc events ----

// trigger_event(player, "NpcName::OnLabel" [, ontouch]) -> 1 on dispatch
static int lw_trigger_event(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    const char* ev   = luaL_checkstring(L, 2);
    int         touch = (int)luaL_optinteger(L, 3, 0);
    lua_pushinteger(L, g_api->npc.event(sd, ev, touch));
    return 1;
}

// ---- clif effects / chat ----

// progressbar(player, color_rgb, seconds)
static int lw_progressbar(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    uint32_t color = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t secs  = (uint32_t)luaL_checkinteger(L, 3);
    g_api->clif.progressbar(sd, color, secs);
    return 0;
}

// progressbar_abort(player)
static int lw_progressbar_abort(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    g_api->clif.progressbar_abort(sd);
    return 0;
}

// messagecolor(player, color_rgb, message [, target])
//   target: 0=ALL_CLIENT, 1=ALL_SAMEMAP, 2=AREA, 3=AREA_WOS, 24=SELF
static int lw_messagecolor(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    uint32_t color = (uint32_t)luaL_checkinteger(L, 2);
    const char* m  = luaL_checkstring(L, 3);
    int32_t target = (int32_t)luaL_optinteger(L, 4, 24); // SELF
    g_api->clif.messagecolor(g_api->pc.as_bl(sd), color, m, true, target);
    return 0;
}

// specialeffect_single(player, effect_id) — sends only to that player
static int lw_specialeffect_single(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t eff = (int32_t)luaL_checkinteger(L, 2);
    g_api->clif.specialeffect_single(g_api->pc.as_bl(sd), eff,
                                      g_api->pc.get_fd(sd));
    return 0;
}

// mapannounce("mapname", "text" [, color]) — broadcast to one map only
//
// clif.broadcast(bl, ..., ALL_SAMEMAP) sends to every player on bl's map,
// so we just need ANY anchor on the map. We iterate until we find the
// first player, broadcast through it, then stop visiting.
struct MapAnnounceCtx { const char* msg; int color; bool sent; };
static int32_t map_announce_cb(block_list* bl, void* user) {
    auto* c = static_cast<MapAnnounceCtx*>(user);
    if (c->sent) return 0;
    g_api->clif.broadcast(bl, c->msg, c->color, 1); // 1 = ALL_SAMEMAP
    c->sent = true;
    return 1;
}

static int lw_mapannounce(lua_State* L) {
    const char* m   = luaL_checkstring(L, 1);
    const char* msg = luaL_checkstring(L, 2);
    int color       = (int)luaL_optinteger(L, 3, 0);
    int16_t mid     = (int16_t)g_api->map.name2id(m);

    MapAnnounceCtx ctx{msg, color, false};
    g_api->map.foreachinmap(map_announce_cb, &ctx, mid, 1 << PLUGIN_BL_PC);
    lua_pushinteger(L, ctx.sent ? 1 : 0);
    return 1;
}

// ---- time helpers ----

// gettime(unit) — 1=sec 2=min 3=hour 4=wday(1..7) 5=mday 6=mon(1..12) 7=year 8=yday(1..366)
static int lw_gettime(lua_State* L) {
    int unit = (int)luaL_checkinteger(L, 1);
    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);
    int v = 0;
    switch (unit) {
        case 1: v = lt.tm_sec;          break;
        case 2: v = lt.tm_min;          break;
        case 3: v = lt.tm_hour;         break;
        case 4: v = lt.tm_wday + 1;     break;
        case 5: v = lt.tm_mday;         break;
        case 6: v = lt.tm_mon + 1;      break;
        case 7: v = lt.tm_year + 1900;  break;
        case 8: v = lt.tm_yday + 1;     break;
        default: v = 0;                 break;
    }
    lua_pushinteger(L, v);
    return 1;
}

// gettimestr("%Y-%m-%d %H:%M:%S" [, length=64]) -> formatted string
static int lw_gettimestr(lua_State* L) {
    const char* fmt = luaL_checkstring(L, 1);
    int len         = (int)luaL_optinteger(L, 2, 64);
    if (len < 1)   len = 1;
    if (len > 512) len = 512;
    std::vector<char> buf(len + 1);
    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);
    strftime(buf.data(), buf.size(), fmt, &lt);
    lua_pushstring(L, buf.data());
    return 1;
}

// getservertime() -> unix timestamp (seconds since epoch)
static int lw_getservertime(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)time(nullptr));
    return 1;
}

// ---- pc accessors / inventory queries ----

// countitem(player, item_id) -> total stacks summed
static int lw_countitem(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    uint32_t id = (uint32_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, g_api->pc.countitem(sd, id));
    return 1;
}

// read_param(player, sp_type) -> int (Str=13, Agi=14, ...; SP_* constants)
static int lw_read_param(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, (lua_Integer)g_api->pc.read_param(sd, type));
    return 1;
}

// get_equip_id(player, eqi_slot) -> nameid (0 if empty)
static int lw_get_equip_id(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushinteger(L, 0); return 1; }
    int32_t slot = (int32_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, g_api->pc.get_equip_nameid(sd, slot));
    return 1;
}

// ---- stat bonuses ----

// bonus(player, sp_type, val)
static int lw_bonus(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    int32_t val  = (int32_t)luaL_checkinteger(L, 3);
    g_api->pc.bonus(sd, type, val);
    return 0;
}

// bonus2(player, type, v1, v2)
static int lw_bonus2(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    int32_t v1   = (int32_t)luaL_checkinteger(L, 3);
    int32_t v2   = (int32_t)luaL_checkinteger(L, 4);
    g_api->pc.bonus2(sd, type, v1, v2);
    return 0;
}

// bonus3(player, type, v1, v2, v3)
static int lw_bonus3(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    int32_t v1   = (int32_t)luaL_checkinteger(L, 3);
    int32_t v2   = (int32_t)luaL_checkinteger(L, 4);
    int32_t v3   = (int32_t)luaL_checkinteger(L, 5);
    g_api->pc.bonus3(sd, type, v1, v2, v3);
    return 0;
}

// bonus4(player, type, v1, v2, v3, v4)
static int lw_bonus4(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    int32_t v1   = (int32_t)luaL_checkinteger(L, 3);
    int32_t v2   = (int32_t)luaL_checkinteger(L, 4);
    int32_t v3   = (int32_t)luaL_checkinteger(L, 5);
    int32_t v4   = (int32_t)luaL_checkinteger(L, 6);
    g_api->pc.bonus4(sd, type, v1, v2, v3, v4);
    return 0;
}

// bonus5(player, type, v1, v2, v3, v4, v5)
static int lw_bonus5(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    int32_t type = (int32_t)luaL_checkinteger(L, 2);
    int32_t v1   = (int32_t)luaL_checkinteger(L, 3);
    int32_t v2   = (int32_t)luaL_checkinteger(L, 4);
    int32_t v3   = (int32_t)luaL_checkinteger(L, 5);
    int32_t v4   = (int32_t)luaL_checkinteger(L, 6);
    int32_t v5   = (int32_t)luaL_checkinteger(L, 7);
    g_api->pc.bonus5(sd, type, v1, v2, v3, v4, v5);
    return 0;
}

// ---- variable storage (setd / getd / array) ----
//
// Variable scope is determined by the prefix of the name:
//   .       NPC-scope             .@      local stack frame
//   #       char-shared           ##      account-wide
//   @       temporary char        $       global permanent
//   $@      global temporary      '       instance-scoped
//
// String variables use a trailing '$' (`$@name$` is a string).
// The `index` argument is the array index; pass 0 for non-array vars.
//
// All var ops require a script_state, so they must be called from inside
// a register_buildin handler (where we have an active coroutine).

static int lw_set_var(lua_State* L) {
    if (!g_active_coro) {
        return luaL_error(L,
            "set_var must be called from a register_buildin handler");
    }
    map_session_data* sd =
        lua_isnoneornil(L, 1) ? nullptr : sd_from_arg(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int32_t index    = (int32_t)luaL_optinteger(L, 4, 0);

    if (lua_isinteger(L, 3) || lua_isnumber(L, 3)) {
        g_api->script.set_var_num(g_active_coro->st, sd, name, index,
                                  lua_tointeger(L, 3));
    } else {
        const char* v = luaL_checkstring(L, 3);
        g_api->script.set_var_str(g_active_coro->st, sd, name, index, v);
    }
    return 0;
}

static int lw_get_var(lua_State* L) {
    if (!g_active_coro) {
        return luaL_error(L,
            "get_var must be called from a register_buildin handler");
    }
    map_session_data* sd =
        lua_isnoneornil(L, 1) ? nullptr : sd_from_arg(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int32_t index    = (int32_t)luaL_optinteger(L, 3, 0);

    // The trailing-$ convention selects the storage slot:
    //   `$@flag`  -> integer
    //   `$@name$` -> string
    size_t len = strlen(name);
    bool is_str = (len > 0 && name[len - 1] == '$');
    if (is_str) {
        const char* v =
            g_api->script.get_var_str(g_active_coro->st, sd, name, index);
        lua_pushstring(L, v ? v : "");
    } else {
        lua_pushinteger(L,
            (lua_Integer)g_api->script.get_var_num(
                g_active_coro->st, sd, name, index));
    }
    return 1;
}

// ---- NPC dialog (mes / next / menu / input / close) ----
//
// `mes(player, "...")` just sends a chat-window line and does not block;
// queue several mes lines, then call one of the blocking primitives:
//
//   next_dialog(player)        wait for the player to click "Next"
//   close_dialog(player)       wait for the player to click "Close"
//   menu(player, "a:b:c")      wait for menu selection (read npc_menu(player))
//   input_int(player)          wait for integer input  (read npc_amount(player))
//   input_str(player)          wait for string input   (read npc_str(player))
//
// The blocking primitives suspend the calling NPC script and yield the
// Lua coroutine. The engine resumes the script_state when the player
// responds — the *next* NPC script command runs (typically another Lua
// register_buildin command that reads the response and continues the
// dialog). The Lua function the dialog primitive returns from does NOT
// resume; any Lua code after it is unreachable by design.

static int32_t resolve_dialog_oid(map_session_data* sd) {
    return g_api->pc.get_npc_id(sd);
}

// mes(player, "text") — non-blocking line
static int lw_mes(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    const char* msg = luaL_checkstring(L, 2);
    g_api->clif.scriptmes(sd, resolve_dialog_oid(sd), msg);
    return 0;
}

// Internal: park the calling script and yield the coroutine so the engine
// owns resumption. `g_active_coro` must be valid (i.e. we're inside a
// register_buildin handler).
static int dialog_park_and_yield(lua_State* L, const char* fnname) {
    if (!g_active_coro) {
        return luaL_error(L,
            "%s must be called from a register_buildin handler", fnname);
    }
    g_api->script.suspend(g_active_coro->st);
    g_active_coro->engine_resumed = true;
    return lua_yield(L, 0);
}

// next_dialog(player) — sends "Next" prompt; engine resumes script after click
static int lw_next_dialog(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    g_api->clif.scriptnext(sd, resolve_dialog_oid(sd));
    return dialog_park_and_yield(L, "next_dialog");
}

// close_dialog(player) — sends "Close" button
static int lw_close_dialog(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    g_api->clif.scriptclose(sd, resolve_dialog_oid(sd));
    return dialog_park_and_yield(L, "close_dialog");
}

// menu(player, "Buy:Sell:Cancel")
static int lw_menu(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    const char* options = luaL_checkstring(L, 2);
    g_api->clif.scriptmenu(sd, resolve_dialog_oid(sd), options);
    return dialog_park_and_yield(L, "menu");
}

// input_int(player)
static int lw_input_int(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    g_api->clif.scriptinput(sd, resolve_dialog_oid(sd));
    return dialog_park_and_yield(L, "input_int");
}

// input_str(player)
static int lw_input_str(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) return 0;
    g_api->clif.scriptinputstr(sd, resolve_dialog_oid(sd));
    return dialog_park_and_yield(L, "input_str");
}

// ---- NPC response readers (call from the *follow-up* buildin) ----

// npc_oid(player) -> bl id of the NPC the player is interacting with
static int lw_npc_oid(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    lua_pushinteger(L, sd ? g_api->pc.get_npc_id(sd) : 0);
    return 1;
}

// npc_menu(player) -> 1-based menu index the player picked (0 if none)
static int lw_npc_menu(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    lua_pushinteger(L, sd ? g_api->pc.get_npc_menu(sd) : 0);
    return 1;
}

// npc_amount(player) -> integer the player typed
static int lw_npc_amount(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    lua_pushinteger(L, sd ? g_api->pc.get_npc_amount(sd) : 0);
    return 1;
}

// npc_str(player) -> string the player typed
static int lw_npc_str(lua_State* L) {
    map_session_data* sd = sd_from_arg(L, 1);
    if (!sd) { lua_pushstring(L, ""); return 1; }
    const char* s = g_api->pc.get_npc_str(sd);
    lua_pushstring(L, s ? s : "");
    return 1;
}

// ---- timer support ----

struct LuaTimer {
    int ref; // luaL_ref of the Lua callback
};

static int32_t lua_timer_dispatch(int32_t /*tid*/, int64_t /*tick*/,
                                  int32_t id, intptr_t data) {
    auto* tm = reinterpret_cast<LuaTimer*>(data);
    if (!tm) return 0;
    lua_State* L = L_get();
    if (L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, tm->ref);
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, id);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                wlog_warning("timer error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, tm->ref);
    }
    delete tm;
    return 0;
}

// timer_after(delay_ms, fn [, id]) -> tid
static int lw_timer_after(lua_State* L) {
    int64_t delay = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int32_t id = (int32_t)luaL_optinteger(L, 3, 0);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto* tm = new LuaTimer{ref};
    int64_t when = g_api->timer.gettick() + delay;
    int32_t tid = g_api->timer.add_timer(when, lua_timer_dispatch, id,
                                          reinterpret_cast<intptr_t>(tm));
    lua_pushinteger(L, tid);
    return 1;
}

// ---- OnClock / OnMinute / OnHour / OnDay / OnSun..OnSat events ----
//
// Names follow the rAthena label convention so users can copy-paste from
// existing scripts. The dispatcher runs every second once start_event_system
// has been called; it fires events when the wall-clock minute, hour, or
// day rolls over (matching npc_event_do_clock semantics).

// event-name → list of registry refs (Lua functions)
static std::map<std::string, std::vector<int>> g_event_handlers;

static int32_t g_clock_tid = -1;
static struct tm g_prev_tm = {};

static void fire_event(const char* name) {
    auto it = g_event_handlers.find(name);
    if (it == g_event_handlers.end()) return;
    lua_State* L = L_get();
    if (!L) return;

    // Copy the ref list — handlers are allowed to (un)register events
    // during dispatch.
    std::vector<int> refs = it->second;
    for (int ref : refs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushstring(L, name);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            wlog_warning("event '%s' error: %s", name,
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

static int32_t clock_tick_cb(int32_t /*tid*/, int64_t /*tick*/,
                             int32_t /*id*/, intptr_t /*data*/) {
    time_t now = time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);

    char buf[32];

    if (lt.tm_min != g_prev_tm.tm_min) {
        snprintf(buf, sizeof(buf), "OnMinute%02d", lt.tm_min);
        fire_event(buf);

        snprintf(buf, sizeof(buf), "OnClock%02d%02d", lt.tm_hour, lt.tm_min);
        fire_event(buf);

        const char* day = nullptr;
        switch (lt.tm_wday) {
            case 0: day = "OnSun"; break;
            case 1: day = "OnMon"; break;
            case 2: day = "OnTue"; break;
            case 3: day = "OnWed"; break;
            case 4: day = "OnThu"; break;
            case 5: day = "OnFri"; break;
            case 6: day = "OnSat"; break;
        }
        if (day) {
            snprintf(buf, sizeof(buf), "%s%02d%02d", day, lt.tm_hour, lt.tm_min);
            fire_event(buf);
        }
    }

    if (lt.tm_hour != g_prev_tm.tm_hour) {
        snprintf(buf, sizeof(buf), "OnHour%02d", lt.tm_hour);
        fire_event(buf);
    }

    if (lt.tm_mday != g_prev_tm.tm_mday) {
        snprintf(buf, sizeof(buf), "OnDay%02d%02d",
                 lt.tm_mon + 1, lt.tm_mday);
        fire_event(buf);
    }

    g_prev_tm = lt;
    return 0;
}

// on_event("OnClock1300", fn) — generic registration. Accepts any of the
// rAthena clock label names (OnMinuteMM, OnHourHH, OnClockHHMM, OnDayMMDD,
// OnSun..SatHHMM). Multiple handlers may register for the same name.
static int lw_on_event(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_event_handlers[name].push_back(ref);
    return 0;
}

// on_clock(hh, mm, fn) — fires once daily at HH:MM
static int lw_on_clock(lua_State* L) {
    int hh = (int)luaL_checkinteger(L, 1);
    int mm = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    char buf[32];
    snprintf(buf, sizeof(buf), "OnClock%02d%02d", hh, mm);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_event_handlers[buf].push_back(ref);
    return 0;
}

// on_minute(mm, fn) — fires every hour at xx:MM
static int lw_on_minute(lua_State* L) {
    int mm = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    char buf[32];
    snprintf(buf, sizeof(buf), "OnMinute%02d", mm);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_event_handlers[buf].push_back(ref);
    return 0;
}

// on_hour(hh, fn) — fires daily at HH:00
static int lw_on_hour(lua_State* L) {
    int hh = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    char buf[32];
    snprintf(buf, sizeof(buf), "OnHour%02d", hh);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_event_handlers[buf].push_back(ref);
    return 0;
}

// on_day(month, day, fn) — fires yearly on month/day at 00:00
static int lw_on_day(lua_State* L) {
    int mo = (int)luaL_checkinteger(L, 1);
    int dd = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    char buf[32];
    snprintf(buf, sizeof(buf), "OnDay%02d%02d", mo, dd);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_event_handlers[buf].push_back(ref);
    return 0;
}

// ---- Named relative timers (mirror NPC OnTimer<ms>) ----
//
// Lua side:
//   on_timer("boss", 5000, function() ... end)   -- registers offset
//   on_timer("boss", 10000, function() ... end)  -- can register many
//   init_timer("boss")                           -- clears any pending
//   start_timer("boss")                          -- begins ticking
//   stop_timer("boss")                           -- pauses
//   get_timer_tick("boss")                       -- ms elapsed since start

struct NamedTimerCb {
    int64_t offset_ms;
    int     ref;
    int32_t scheduled_tid = -1;
};

struct NamedTimer {
    bool                       running    = false;
    int64_t                    start_tick = 0;
    std::vector<NamedTimerCb>  callbacks;
};

static std::map<std::string, NamedTimer> g_named_timers;

// One heap object per scheduled tick so the timer callback can identify
// which (timer, callback-index) it belongs to without using a raw index
// that might be invalidated by reallocations.
struct ScheduledNamedTimer {
    std::string timer_name;
    size_t      cb_index;
};

static int32_t named_timer_cb(int32_t /*tid*/, int64_t /*tick*/,
                              int32_t /*id*/, intptr_t data) {
    auto* sched = reinterpret_cast<ScheduledNamedTimer*>(data);
    if (!sched) return 0;

    auto it = g_named_timers.find(sched->timer_name);
    if (it != g_named_timers.end()
        && sched->cb_index < it->second.callbacks.size()) {
        auto& cb = it->second.callbacks[sched->cb_index];
        cb.scheduled_tid = -1;

        lua_State* L = L_get();
        if (L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, sched->timer_name.c_str());
                lua_pushinteger(L, (lua_Integer)cb.offset_ms);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    wlog_warning("OnTimer '%s' error: %s",
                                 sched->timer_name.c_str(),
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
        }
    }
    delete sched;
    return 0;
}

static void cancel_pending_named_timer(NamedTimer& nt) {
    for (auto& cb : nt.callbacks) {
        if (cb.scheduled_tid >= 0) {
            g_api->timer.delete_timer(cb.scheduled_tid, named_timer_cb);
            cb.scheduled_tid = -1;
        }
    }
}

// on_timer(name, offset_ms, fn)
static int lw_on_timer(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int64_t off      = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto& nt = g_named_timers[name];
    nt.callbacks.push_back({off, ref, -1});
    return 0;
}

// init_timer(name): cancel pending, reset to a stopped state at 0.
static int lw_init_timer(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto& nt = g_named_timers[name];
    cancel_pending_named_timer(nt);
    nt.running = false;
    nt.start_tick = 0;
    return 0;
}

// start_timer(name): begin ticking; schedule each callback's offset.
// If the timer is already running this is a no-op.
static int lw_start_timer(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto& nt = g_named_timers[name];
    if (nt.running) return 0;
    nt.running    = true;
    nt.start_tick = g_api->timer.gettick();
    for (size_t i = 0; i < nt.callbacks.size(); ++i) {
        auto& cb = nt.callbacks[i];
        auto* sched = new ScheduledNamedTimer{name, i};
        cb.scheduled_tid = g_api->timer.add_timer(
            nt.start_tick + cb.offset_ms,
            named_timer_cb, 0,
            reinterpret_cast<intptr_t>(sched));
    }
    return 0;
}

// stop_timer(name): pause; cancel pending callbacks.
static int lw_stop_timer(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto it = g_named_timers.find(name);
    if (it == g_named_timers.end()) return 0;
    cancel_pending_named_timer(it->second);
    it->second.running = false;
    return 0;
}

// get_timer_tick(name) -> ms elapsed since last start, or 0 when stopped
static int lw_get_timer_tick(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto it = g_named_timers.find(name);
    if (it == g_named_timers.end() || !it->second.running) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L,
        (lua_Integer)(g_api->timer.gettick() - it->second.start_tick));
    return 1;
}

// ---- atcommand registration ----

static int32_t atcmd_dispatch(map_session_data* sd, const char* /*command*/,
                              const char* message, void* user_data) {
    auto* reg = static_cast<LuaAtcmd*>(user_data);
    if (!reg) return 0;
    lua_State* L = L_get();
    if (!L) return 0;

    lua_rawgeti(L, LUA_REGISTRYINDEX, reg->ref);
    if (!lua_isfunction(L, -1)) {
        // Stale ref (e.g. mid-reload). Treat as not handled.
        lua_pop(L, 1);
        return 0;
    }
    push_player(L, sd);
    lua_pushstring(L, message ? message : "");
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        wlog_warning("atcmd '%s' error: %s", reg->name.c_str(),
                     lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    int rc = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return rc;
}

// register_atcmd("name", level, fn)
//   fn(player, args_string) -> int
//
// Re-registering the same name overwrites the existing handler; the
// underlying record (and the engine's binding to it) stays put across
// reloads, so we don't need to ask the engine to unregister.
static int lw_register_atcmd(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int level = (int)luaL_optinteger(L, 2, 0);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto it = g_atcmd_by_name.find(name);
    if (it != g_atcmd_by_name.end()) {
        // Reload path: drop the old function ref, keep the user_data pointer
        // the engine already holds.
        luaL_unref(L, LUA_REGISTRYINDEX, it->second->ref);
        it->second->ref = new_ref;
        lua_pushboolean(L, 1);
        return 1;
    }

    auto* reg = new LuaAtcmd{new_ref, name};
    g_atcmd_by_name[name] = reg;
    bool ok = g_api->atcmd.register_cmd(name, level, atcmd_dispatch, reg);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ---- script command registration ----
//
// Lua handlers run inside a coroutine so they can call sleep() or
// script_suspend() to pause the calling NPC script. When the coroutine
// finishes (LUA_OK), its top return value is pushed back to the script
// stack and (if it had suspended) script.resume() re-enters execution.
// When it yields, the script_state stays parked until something else
// drives the coroutine forward (e.g. a sleep timer).

// Active coroutine for the currently-running Lua wrapper. Wrappers like
// sleep() consult `g_active_coro` (declared near the top of the file) to
// know which script_state to suspend.

// Push a value from coro->T's stack back onto the calling script_state.
static void push_coro_return_to_script(BuildinCoro* coro, int nresults) {
    if (nresults <= 0) {
        g_api->script.pushint(coro->st, 0);
        return;
    }
    int top = lua_gettop(coro->T);
    if (lua_isinteger(coro->T, top) || lua_isnumber(coro->T, top)) {
        g_api->script.pushint(coro->st, lua_tointeger(coro->T, top));
    } else if (lua_isstring(coro->T, top)) {
        g_api->script.pushstr(coro->st, lua_tostring(coro->T, top));
    } else {
        g_api->script.pushint(coro->st, 0);
    }
}

static void cleanup_coro(BuildinCoro* coro) {
    if (!coro) return;
    if (lua_State* L = L_get()) {
        if (coro->thread_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, coro->thread_ref);
        }
    }
    delete coro;
}

// Drive a coroutine forward (initial entry or after yield). Pushes the
// return value to the script stack and resumes the parked NPC script when
// the coroutine completes (or errors).
static void drive_coro(BuildinCoro* coro, int nargs) {
    g_active_coro = coro;
    int nresults = 0;
    int rc = lua_resume(coro->T, nullptr, nargs, &nresults);
    g_active_coro = nullptr;

    if (rc == LUA_YIELD) {
        // Two flavours of yield:
        //
        //   1. sleep-style — the wrapper scheduled a timer that calls back
        //      into drive_coro and the Lua function continues. Keep the
        //      coroutine alive.
        //   2. dialog-style — the wrapper called clif.script* + script.suspend
        //      because the player needs to respond. The engine resumes the
        //      script_state itself via npc_scriptcont; our Lua coroutine is
        //      not meant to continue (the rest of the dialog flow lives in
        //      whatever NPC script command runs after this one). Tear down
        //      the coroutine cleanly and don't double-resume the script.
        if (coro->engine_resumed) {
            cleanup_coro(coro);
        }
        return;
    }

    if (rc != LUA_OK) {
        wlog_error("buildin '%s' error: %s", coro->name.c_str(),
                   lua_tostring(coro->T, -1));
        g_api->script.pushint(coro->st, 0);
    } else {
        push_coro_return_to_script(coro, nresults);
    }

    void* token = coro->token;
    cleanup_coro(coro);

    // Re-enter the script engine if we had parked it. resume() is a safe
    // no-op if the script_state was freed in the meantime (e.g. logout).
    if (token) g_api->script.resume(token);
}

static int32_t buildin_dispatch(script_state* st, void* user_data) {
    auto* reg = static_cast<LuaScriptCmd*>(user_data);
    if (!reg) return PLUGIN_SCRIPT_CMD_FAILURE;
    lua_State* L = L_get();
    if (!L) return PLUGIN_SCRIPT_CMD_FAILURE;

    auto* coro = new BuildinCoro();
    coro->st   = st;
    coro->name = reg->name;
    coro->T    = lua_newthread(L);
    coro->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_rawgeti(coro->T, LUA_REGISTRYINDEX, reg->ref);
    if (!lua_isfunction(coro->T, -1)) {
        // Stale ref (e.g. mid-reload). Push 0 and bail without driving.
        lua_pop(coro->T, 1);
        cleanup_coro(coro);
        g_api->script.pushint(st, 0);
        return PLUGIN_SCRIPT_CMD_SUCCESS;
    }

    map_session_data* sd = g_api->script.rid2sd(st);
    push_player(coro->T, sd);

    int nargs = 1; // player table
    const std::string& spec = reg->spec;
    int n = (int)spec.size();
    for (int i = 0; i < n; ++i) {
        char c = spec[i];
        if (c == '?' || c == '*') break;
        int slot = 2 + i;
        if (!g_api->script.hasdata(st, slot)) break;
        if (c == 'i') {
            lua_pushinteger(coro->T, g_api->script.getnum(st, slot));
        } else {
            const char* s = g_api->script.getstr(st, slot);
            lua_pushstring(coro->T, s ? s : "");
        }
        ++nargs;
    }

    drive_coro(coro, nargs);
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// ---- async wrappers (must be called inside a buildin coroutine) ----

// sleep(ms): pauses the calling NPC script for `ms` milliseconds.
// Internally suspends the script_state and yields the Lua coroutine; a
// timer fires after ms and resumes the coroutine, which then resumes the
// script when it returns.
static int32_t sleep_resume_cb(int32_t /*tid*/, int64_t /*tick*/,
                               int32_t /*id*/, intptr_t data) {
    auto* coro = reinterpret_cast<BuildinCoro*>(data);
    if (!coro) return 0;
    drive_coro(coro, 0);
    return 0;
}

static int lw_sleep(lua_State* L) {
    int64_t ms = luaL_checkinteger(L, 1);
    if (!g_active_coro) {
        return luaL_error(L,
            "sleep() must be called from a register_buildin handler");
    }
    if (!g_active_coro->token) {
        g_active_coro->token = g_api->script.suspend(g_active_coro->st);
    }
    int64_t when = g_api->timer.gettick() + ms;
    g_api->timer.add_timer(when, sleep_resume_cb, 0,
                           reinterpret_cast<intptr_t>(g_active_coro));
    return lua_yield(L, 0);
}

// script_suspend() -> opaque token (light userdata).
//
// Low-level escape hatch for advanced patterns: pause the calling script
// without yielding the Lua coroutine. The caller becomes responsible for
// arranging a future script_resume(token, value).
static int lw_script_suspend(lua_State* L) {
    if (!g_active_coro) {
        return luaL_error(L,
            "script_suspend() must be called from a register_buildin handler");
    }
    if (!g_active_coro->token) {
        g_active_coro->token = g_api->script.suspend(g_active_coro->st);
    }
    lua_pushlightuserdata(L, g_active_coro->token);
    return 1;
}

// script_resume(token [, value])
//
// Resumes a previously-suspended script, optionally pushing a return value
// (number or string) before re-entering execution. Safe no-op if the
// token is null or refers to a freed script_state.
static int lw_script_resume(lua_State* L) {
    void* token = lua_isuserdata(L, 1) ? lua_touserdata(L, 1) : nullptr;
    if (!token) return 0;

    auto* st = static_cast<script_state*>(token);
    if (lua_isinteger(L, 2) || lua_isnumber(L, 2)) {
        g_api->script.pushint(st, lua_tointeger(L, 2));
    } else if (lua_isstring(L, 2)) {
        g_api->script.pushstr(st, lua_tostring(L, 2));
    }
    g_api->script.resume(token);
    return 0;
}

// register_buildin("name", "argspec", fn)
//   argspec follows rAthena conventions: 'i' = number, 's' = string.
//   fn(player, arg1, arg2, ...) is called when the command runs.
//
// Re-registering the same name updates the bound Lua function while
// keeping the engine-side registration intact. There's no longer a
// hard cap on the number of registered buildins.
static int lw_register_buildin(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* spec = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto it = g_buildin_by_name.find(name);
    if (it != g_buildin_by_name.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second->ref);
        it->second->ref  = new_ref;
        it->second->spec = spec;   // tolerate spec changes between reloads
        lua_pushboolean(L, 1);
        return 1;
    }

    auto* reg = new LuaScriptCmd{new_ref, name, spec};
    g_buildin_by_name[name] = reg;
    bool ok = g_api->script_addcommand(name, spec, buildin_dispatch, reg);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ---- hook registration ----

// Map of hook ids to a generic dispatcher that also pushes useful context.
static int hook_event_id_from_name(const std::string& s) {
    if (s == "pc_login")        return HOOK_PC_LOGIN;
    if (s == "pc_logout")       return HOOK_PC_LOGOUT;
    if (s == "pc_baselevelup")  return HOOK_PC_BASELEVELUP;
    if (s == "pc_joblevelup")   return HOOK_PC_JOBLEVELUP;
    if (s == "pc_dead")         return HOOK_PC_DEAD;
    if (s == "pc_chat")         return HOOK_PC_CHAT;
    if (s == "pc_whisper")      return HOOK_PC_WHISPER;
    if (s == "mob_kill")        return HOOK_MOB_KILL;
    if (s == "mob_spawn")       return HOOK_MOB_SPAWN;
    if (s == "item_use")        return HOOK_ITEM_USE;
    if (s == "item_pickup")     return HOOK_ITEM_PICKUP;
    if (s == "item_drop")       return HOOK_ITEM_DROP;
    if (s == "item_equip")      return HOOK_ITEM_EQUIP;
    if (s == "skill_use")       return HOOK_SKILL_USE;
    if (s == "npc_click")       return HOOK_NPC_CLICK;
    if (s == "atcmd_execute")   return HOOK_ATCMD_EXECUTE;
    if (s == "quest_add")       return HOOK_QUEST_ADD;
    if (s == "quest_complete")  return HOOK_QUEST_COMPLETE;
    if (s == "storage_open")    return HOOK_STORAGE_OPEN;
    return -1;
}

static int hook_dispatch(void* data, void* user_data) {
    int hook_type = (int)(intptr_t)user_data;
    lua_State* L = L_get();
    if (!L) return HOOK_CONTINUE;

    int rc_default = HOOK_CONTINUE;

    // Find every Lua callback bound to this hook_type and invoke them.
    for (auto& h : g_hook_callbacks) {
        if (h.hook_type != hook_type) continue;

        lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
        // Push a context table tailored to the hook type. This is best-effort —
        // hooks not enumerated here just get an empty table.
        lua_newtable(L);

        switch (hook_type) {
        case HOOK_PC_LOGIN: case HOOK_PC_LOGOUT:
        case HOOK_PC_BASELEVELUP: case HOOK_PC_JOBLEVELUP:
        case HOOK_STORAGE_OPEN: {
            auto* d = static_cast<plugin_pc_login_t*>(data);
            push_player(L, d->sd);
            lua_setfield(L, -2, "player");
            break;
        }
        case HOOK_PC_DEAD: {
            auto* d = static_cast<plugin_pc_dead_t*>(data);
            push_player(L, d->sd);
            lua_setfield(L, -2, "player");
            break;
        }
        case HOOK_PC_CHAT: {
            auto* d = static_cast<plugin_pc_chat_t*>(data);
            push_player(L, d->sd);
            lua_setfield(L, -2, "player");
            lua_pushstring(L, d->message ? d->message : "");
            lua_setfield(L, -2, "message");
            break;
        }
        case HOOK_MOB_KILL: {
            auto* d = static_cast<plugin_mob_kill_t*>(data);
            if (d->md) {
                lua_pushinteger(L, g_api->mob.get_id(d->md));
                lua_setfield(L, -2, "mob_id");
                lua_pushstring(L, g_api->mob.get_name(d->md));
                lua_setfield(L, -2, "mob_name");
            }
            if (d->src && g_api->bl.get_type(d->src) == PLUGIN_BL_PC) {
                push_player(L, g_api->bl.as_sd(d->src));
                lua_setfield(L, -2, "killer");
            }
            break;
        }
        case HOOK_MOB_SPAWN: {
            auto* d = static_cast<plugin_mob_spawn_t*>(data);
            if (d->md) {
                lua_pushinteger(L, g_api->mob.get_id(d->md));
                lua_setfield(L, -2, "mob_id");
                lua_pushstring(L, g_api->mob.get_name(d->md));
                lua_setfield(L, -2, "mob_name");
            }
            break;
        }
        case HOOK_ITEM_PICKUP: {
            auto* d = static_cast<plugin_item_pickup_t*>(data);
            push_player(L, d->sd);
            lua_setfield(L, -2, "player");
            if (d->it) {
                uint32_t id = g_api->item_api.get_nameid(d->it);
                lua_pushinteger(L, id);
                lua_setfield(L, -2, "item_id");
                lua_pushstring(L, g_api->item_api.db_get_ename(id));
                lua_setfield(L, -2, "item_name");
            }
            lua_pushinteger(L, d->amount);
            lua_setfield(L, -2, "amount");
            break;
        }
        case HOOK_ATCMD_EXECUTE: {
            auto* d = static_cast<plugin_atcmd_execute_t*>(data);
            push_player(L, d->sd);
            lua_setfield(L, -2, "player");
            lua_pushstring(L, d->command ? d->command : "");
            lua_setfield(L, -2, "command");
            lua_pushstring(L, d->params ? d->params : "");
            lua_setfield(L, -2, "params");
            break;
        }
        default: break;
        }

        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            wlog_warning("hook error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }

        // A handler can return false (or "stop") to cancel the action.
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            rc_default = HOOK_STOP;
        } else if (lua_isstring(L, -1) &&
                   strcmp(lua_tostring(L, -1), "stop") == 0) {
            rc_default = HOOK_STOP;
        }
        lua_pop(L, 1);
    }
    return rc_default;
}

// hook("event_name", fn) — fn(ctx) where ctx is a table (see docs)
static int lw_hook(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int hook_type = hook_event_id_from_name(name);
    if (hook_type < 0) {
        return luaL_error(L, "unknown hook event '%s'", name);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    bool already_bound = false;
    for (auto& h : g_hook_callbacks) {
        if (h.hook_type == hook_type) { already_bound = true; break; }
    }
    g_hook_callbacks.push_back({hook_type, ref});

    if (!already_bound) {
        g_api->hook_add(hook_type, hook_dispatch,
                        (void*)(intptr_t)hook_type, 100);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ---- DB store access ----

// Recursive YAML → Lua converter. Scalars try int → number → bool → string
// in that order so item ids stay numeric, prices stay numeric, but
// AegisName lands as a Lua string. Sequences come back as 1-indexed
// arrays; maps as keyed tables.
static void push_yaml_node(lua_State* L, const YAML::Node& n) {
    if (!n) {
        lua_pushnil(L);
        return;
    }
    if (n.IsNull()) {
        lua_pushnil(L);
        return;
    }
    if (n.IsScalar()) {
        try {
            int64_t i = n.as<int64_t>();
            lua_pushinteger(L, i);
            return;
        } catch (...) {}
        try {
            double d = n.as<double>();
            lua_pushnumber(L, d);
            return;
        } catch (...) {}
        // Booleans only via the literal forms yaml-cpp recognises.
        try {
            bool b = n.as<bool>();
            const std::string& raw = n.Scalar();
            if (raw == "true" || raw == "True"  || raw == "TRUE"  ||
                raw == "false"|| raw == "False" || raw == "FALSE" ||
                raw == "yes"  || raw == "Yes"   || raw == "no"    ||
                raw == "No") {
                lua_pushboolean(L, b ? 1 : 0);
                return;
            }
        } catch (...) {}
        const std::string& s = n.Scalar();
        lua_pushlstring(L, s.data(), s.size());
        return;
    }
    if (n.IsSequence()) {
        lua_newtable(L);
        int i = 1;
        for (auto child : n) {
            push_yaml_node(L, child);
            lua_rawseti(L, -2, i++);
        }
        return;
    }
    if (n.IsMap()) {
        lua_newtable(L);
        for (auto kv : n) {
            std::string k = kv.first.as<std::string>();
            push_yaml_node(L, kv.second);
            lua_setfield(L, -2, k.c_str());
        }
        return;
    }
    lua_pushnil(L);
}

// Read a DB key argument: a Lua number is rendered to its decimal form
// (matching how numeric `Id` fields are stored), a string is used as-is.
static std::string db_key_from_arg(lua_State* L, int idx) {
    if (lua_isinteger(L, idx)) {
        return std::to_string((long long)lua_tointeger(L, idx));
    }
    if (lua_isnumber(L, idx)) {
        // Non-integer numbers don't make sensible keys; truncate.
        return std::to_string((long long)lua_tointeger(L, idx));
    }
    const char* s = luaL_checkstring(L, idx);
    return s ? s : "";
}

// Push a stored key back to Lua: all-digit keys (optionally signed) come
// back as integers so item_db consumers keep numeric ids; everything
// else (status names, etc.) comes back as a string.
static void push_db_key(lua_State* L, const std::string& key) {
    if (!key.empty()) {
        size_t i = (key[0] == '-' || key[0] == '+') ? 1 : 0;
        bool all_digits = i < key.size();
        for (; i < key.size(); ++i) {
            if (key[i] < '0' || key[i] > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            lua_pushinteger(L, (lua_Integer)std::stoll(key));
            return;
        }
    }
    lua_pushstring(L, key.c_str());
}

// db_get(type, key) -> table or nil    (key: number or string)
static int lw_db_get(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    std::string key  = db_key_from_arg(L, 2);
    YAML::Node n = workshop::DbStore::instance().get(type, key);
    if (!n) {
        lua_pushnil(L);
        return 1;
    }
    push_yaml_node(L, n);
    return 1;
}

// db_has(type, key) -> bool
static int lw_db_has(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    std::string key  = db_key_from_arg(L, 2);
    lua_pushboolean(L, workshop::DbStore::instance().has(type, key) ? 1 : 0);
    return 1;
}

// db_count(type) -> int
static int lw_db_count(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)workshop::DbStore::instance().count(type));
    return 1;
}

// db_each(type, function(key, entry) ... end)
//   key arrives as a number for numeric-keyed DBs, a string otherwise.
static int lw_db_each(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    workshop::DbStore::instance().each(type,
        [L, ref](const std::string& key, const YAML::Node& entry) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
            push_db_key(L, key);
            push_yaml_node(L, entry);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                wlog_warning("db_each error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        });

    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return 0;
}

// db_types() -> array of every Type seen in any loaded YAML file
static int lw_db_types(lua_State* L) {
    auto v = workshop::DbStore::instance().types();
    lua_newtable(L);
    int i = 1;
    for (auto& s : v) {
        lua_pushstring(L, s.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// ---- client packet hooks ----
//
// on_packet(cmd, fn)        — run `fn` before the engine's clif handler for
//                             `cmd`; returning false / "stop" suppresses it.
// register_packet(cmd, len, fn) — install a handler for an unused packet id.
// packet_read_b/w/l/str(fd, off), packet_rest(fd) — read the incoming packet.
// packet_send(player|nil, bytes, target), packet_send_self(fd, bytes) — send.
//
// The Lua function gets a ctx table: { fd, cmd, player? }. Use ctx.fd with
// the packet_read_* helpers. Lua refs are carried to the engine as the
// callback's user_data; re-registering a cmd swaps the bound function.

static std::map<uint16_t, int> g_packet_filter_refs;
static std::map<uint16_t, int> g_packet_handler_refs;

static void push_packet_ctx(lua_State* L, int32_t fd, map_session_data* sd,
                            uint16_t cmd) {
    lua_newtable(L);
    lua_pushinteger(L, fd);  lua_setfield(L, -2, "fd");
    lua_pushinteger(L, cmd); lua_setfield(L, -2, "cmd");
    if (sd) { push_player(L, sd); lua_setfield(L, -2, "player"); }
}

static int32_t packet_filter_trampoline(int32_t fd, map_session_data* sd,
                                        void* user_data) {
    int ref = (int)(intptr_t)user_data;
    lua_State* L = L_get();
    if (!L) return PLUGIN_PACKET_PASS;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return PLUGIN_PACKET_PASS; }
    push_packet_ctx(L, fd, sd, g_api->packet.read_w(fd, 0));
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        wlog_warning("on_packet filter error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return PLUGIN_PACKET_PASS;
    }
    bool stop = false;
    if (lua_isboolean(L, -1))      stop = !lua_toboolean(L, -1);     // false -> stop
    else if (lua_isstring(L, -1))  stop = strcmp(lua_tostring(L, -1), "stop") == 0;
    // nil / true / number -> pass through to the engine handler
    lua_pop(L, 1);
    return stop ? PLUGIN_PACKET_STOP : PLUGIN_PACKET_PASS;
}

static void packet_handler_trampoline(int32_t fd, map_session_data* sd,
                                      void* user_data) {
    int ref = (int)(intptr_t)user_data;
    lua_State* L = L_get();
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    push_packet_ctx(L, fd, sd, g_api->packet.read_w(fd, 0));
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        wlog_warning("register_packet handler error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// on_packet(cmd, fn) -> bool
static int lw_on_packet(lua_State* L) {
    uint16_t cmd = (uint16_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto it = g_packet_filter_refs.find(cmd);
    if (it != g_packet_filter_refs.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        it->second = new_ref;
    } else {
        g_packet_filter_refs[cmd] = new_ref;
    }
    bool ok = g_api->packet.register_filter(cmd, packet_filter_trampoline,
                                            (void*)(intptr_t)new_ref);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// register_packet(cmd, length, fn) -> bool   (length: bytes incl. cmd, or -1)
static int lw_register_packet(lua_State* L) {
    uint16_t cmd = (uint16_t)luaL_checkinteger(L, 1);
    int16_t  len = (int16_t)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto it = g_packet_handler_refs.find(cmd);
    if (it != g_packet_handler_refs.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        it->second = new_ref;
    } else {
        g_packet_handler_refs[cmd] = new_ref;
    }
    bool ok = g_api->packet.register_handler(cmd, len, packet_handler_trampoline,
                                             (void*)(intptr_t)new_ref);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lw_packet_read_b(lua_State* L) {
    lua_pushinteger(L, g_api->packet.read_b((int32_t)luaL_checkinteger(L, 1),
                                            (int32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lw_packet_read_w(lua_State* L) {
    lua_pushinteger(L, g_api->packet.read_w((int32_t)luaL_checkinteger(L, 1),
                                            (int32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lw_packet_read_l(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_api->packet.read_l((int32_t)luaL_checkinteger(L, 1),
                                                         (int32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lw_packet_read_str(lua_State* L) {
    const char* s = g_api->packet.read_str((int32_t)luaL_checkinteger(L, 1),
                                           (int32_t)luaL_checkinteger(L, 2));
    lua_pushstring(L, s ? s : "");
    return 1;
}
static int lw_packet_rest(lua_State* L) {
    lua_pushinteger(L, g_api->packet.read_rest((int32_t)luaL_checkinteger(L, 1)));
    return 1;
}

// packet_send_self(fd, bytes) — push a raw packet (cmd at offset 0..1) to fd
static int lw_packet_send_self(lua_State* L) {
    int32_t fd = (int32_t)luaL_checkinteger(L, 1);
    size_t  len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    g_api->packet.send_self(fd, data, (int32_t)len);
    return 0;
}

// packet_send(player|nil, bytes [, target]) — clif_send via send_target.
//   target: 0=ALL_CLIENT, 1=ALL_SAMEMAP, 2=AREA, 3=AREA_WOS, 24=SELF, ...
//   player may be nil only when target == 0.
static int lw_packet_send(lua_State* L) {
    map_session_data* sd = lua_isnoneornil(L, 1) ? nullptr : sd_from_arg(L, 1);
    size_t  len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    int32_t target = (int32_t)luaL_optinteger(L, 3, 0);
    g_api->packet.send_target(sd ? g_api->pc.as_bl(sd) : nullptr,
                              data, (int32_t)len, target);
    return 0;
}

} // anonymous namespace

namespace workshop {

void register_globals(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"announce",            lw_announce},
        {"log_info",            lw_log_info},
        {"log_status",          lw_log_status},
        {"log_warn",            lw_log_warning},
        {"log_warning",         lw_log_warning},
        {"log_error",           lw_log_error},
        {"get_player",          lw_get_player},
        {"get_player_by_name",  lw_get_player_by_name},
        {"getitem",             lw_getitem},
        {"gainexp",             lw_gainexp},
        {"warp",                lw_warp},
        {"heal",                lw_heal},
        // -- status changes --
        {"sc_id",               lw_sc_id},
        {"register_sc",         lw_register_sc},
        {"sc_start",            lw_sc_start},
        {"sc_start_from",       lw_sc_start_from},
        {"sc_end",              lw_sc_end},
        {"sc_clear",            lw_sc_clear},
        {"sc_active",           lw_sc_active},
        {"sc_val",              lw_sc_val},
        {"monster",             lw_monster},
        {"message",             lw_message},
        {"emotion",             lw_emotion},
        {"specialeffect",       lw_specialeffect},
        {"item_name",           lw_item_name},
        {"skill_name",          lw_skill_name},
        {"gettick",             lw_gettick},
        // -- inventory --
        {"delitem",             lw_delitem},
        {"item_exists",         lw_item_exists},
        {"item_internal_name",  lw_item_internal_name},
        {"item_type",           lw_item_type},
        // -- money --
        {"payzeny",             lw_payzeny},
        {"give_zeny",           lw_give_zeny},
        // -- combat / skills --
        {"damage",              lw_damage},
        {"use_skill",           lw_use_skill},
        {"get_skill_lv",        lw_get_skill_lv},
        {"skill_id",            lw_skill_id},
        {"skill_inf",           lw_skill_inf},
        // -- battle config --
        {"battle_get",          lw_battle_get},
        {"battle_set",          lw_battle_set},
        {"battle_has",          lw_battle_has},
        // -- map --
        {"mapindex",            lw_mapindex},
        {"mapname",             lw_mapname},
        {"mapflag",             lw_mapflag},
        {"for_each_player_in_map",  lw_for_each_player_in_map},
        {"for_each_player_in_area", lw_for_each_player_in_area},
        {"count_players_in_map",    lw_count_players_in_map},
        {"count_mobs_in_map",       lw_count_mobs_in_map},
        // -- storage --
        {"open_storage",        lw_open_storage},
        // -- quest --
        {"quest_add",           lw_quest_add},
        {"quest_status",        lw_quest_status},
        {"quest_check",         lw_quest_check},
        // -- npc events --
        {"trigger_event",       lw_trigger_event},
        // -- clif effects --
        {"progressbar",         lw_progressbar},
        {"progressbar_abort",   lw_progressbar_abort},
        {"messagecolor",        lw_messagecolor},
        {"specialeffect_single",lw_specialeffect_single},
        {"mapannounce",         lw_mapannounce},
        // -- time --
        {"gettime",             lw_gettime},
        {"gettimestr",          lw_gettimestr},
        {"getservertime",       lw_getservertime},
        // -- pc accessors --
        {"countitem",           lw_countitem},
        {"read_param",          lw_read_param},
        {"get_equip_id",        lw_get_equip_id},
        // -- stat bonuses --
        {"bonus",               lw_bonus},
        {"bonus2",              lw_bonus2},
        {"bonus3",              lw_bonus3},
        {"bonus4",              lw_bonus4},
        {"bonus5",              lw_bonus5},
        // -- variable storage --
        {"set_var",             lw_set_var},
        {"get_var",             lw_get_var},
        // -- NPC dialog --
        {"mes",                 lw_mes},
        {"next_dialog",         lw_next_dialog},
        {"close_dialog",        lw_close_dialog},
        {"menu",                lw_menu},
        {"input_int",           lw_input_int},
        {"input_str",           lw_input_str},
        // -- NPC response --
        {"npc_oid",             lw_npc_oid},
        {"npc_menu",            lw_npc_menu},
        {"npc_amount",          lw_npc_amount},
        {"npc_str",             lw_npc_str},
        // -- workshop DB store --
        {"db_get",              lw_db_get},
        {"db_has",              lw_db_has},
        {"db_count",            lw_db_count},
        {"db_each",             lw_db_each},
        {"db_types",            lw_db_types},
        // -- client packet hooks --
        {"on_packet",           lw_on_packet},
        {"register_packet",     lw_register_packet},
        {"packet_read_b",       lw_packet_read_b},
        {"packet_read_w",       lw_packet_read_w},
        {"packet_read_l",       lw_packet_read_l},
        {"packet_read_str",     lw_packet_read_str},
        {"packet_rest",         lw_packet_rest},
        {"packet_send",         lw_packet_send},
        {"packet_send_self",    lw_packet_send_self},
        {"timer_after",         lw_timer_after},
        {"sleep",               lw_sleep},
        {"script_suspend",      lw_script_suspend},
        {"script_resume",       lw_script_resume},
        {"register_atcmd",      lw_register_atcmd},
        {"register_buildin",    lw_register_buildin},
        {"hook",                lw_hook},
        {"on_event",            lw_on_event},
        {"on_clock",            lw_on_clock},
        {"on_minute",           lw_on_minute},
        {"on_hour",             lw_on_hour},
        {"on_day",              lw_on_day},
        {"on_timer",            lw_on_timer},
        {"init_timer",          lw_init_timer},
        {"start_timer",         lw_start_timer},
        {"stop_timer",          lw_stop_timer},
        {"get_timer_tick",      lw_get_timer_tick},
        {nullptr, nullptr}
    };
    for (const luaL_Reg* r = fns; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
}

void start_event_system() {
    if (g_clock_tid >= 0) return;

    // Snapshot the current wall-clock so the very first tick (~1s away)
    // doesn't immediately fire OnClock/OnHour/OnDay for the current moment.
    time_t now = time(nullptr);
    localtime_r(&now, &g_prev_tm);

    int64_t when = g_api->timer.gettick() + 1000;
    g_clock_tid  = g_api->timer.add_timer_interval(
        when, clock_tick_cb, 0, 0, 1000);
}

void stop_event_system() {
    if (g_clock_tid >= 0) {
        g_api->timer.delete_timer(g_clock_tid, clock_tick_cb);
        g_clock_tid = -1;
    }

    // Cancel any pending named-timer ticks so their data structs are freed.
    for (auto& kv : g_named_timers) {
        cancel_pending_named_timer(kv.second);
    }

    // Drop registry refs we own. The Lua state is typically about to be
    // closed anyway, but explicit cleanup keeps the door open for hot
    // reload paths that recreate the VM in place.
    //
    // Buildin / atcommand records are intentionally NOT freed here:
    // the engine's command tables still point at them as user_data, so
    // re-registration on reload reuses the same record (only the Lua
    // ref is updated). They live until plugin_final.
    if (lua_State* L = L_get()) {
        for (auto& kv : g_event_handlers) {
            for (int r : kv.second) {
                luaL_unref(L, LUA_REGISTRYINDEX, r);
            }
        }
        for (auto& kv : g_named_timers) {
            for (auto& cb : kv.second.callbacks) {
                luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);
            }
        }
        for (auto& kv : g_buildin_by_name) {
            luaL_unref(L, LUA_REGISTRYINDEX, kv.second->ref);
            kv.second->ref = LUA_NOREF;
        }
        for (auto& kv : g_atcmd_by_name) {
            luaL_unref(L, LUA_REGISTRYINDEX, kv.second->ref);
            kv.second->ref = LUA_NOREF;
        }
        for (auto& kv : g_packet_filter_refs)  luaL_unref(L, LUA_REGISTRYINDEX, kv.second);
        for (auto& kv : g_packet_handler_refs) luaL_unref(L, LUA_REGISTRYINDEX, kv.second);
    }
    g_event_handlers.clear();
    g_named_timers.clear();
    g_hook_callbacks.clear();
    // Drop our knowledge of registered plugin SC ids. The engine-side
    // PluginSCDef entries persist (no unregister API), but their calc
    // callbacks reference Lua refs that vanish with the VM, so they go
    // inert; a reload that re-runs register_sc just adds fresh ones.
    g_plugin_sc_ids.clear();
    // Likewise the engine keeps our packet filters/handlers registered with
    // a now-stale ref as user_data; the trampolines no-op on that (the ref
    // resolves to nil in the fresh VM). A reload re-registers cleanly.
    g_packet_filter_refs.clear();
    g_packet_handler_refs.clear();
}

} // namespace workshop
