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
#include "workshop.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ---- registry of Lua callbacks bound to plugin hooks / commands ----
//
// plugin_api expects a C function pointer for every hook & script command,
// but mods register Lua functions. We keep a single trampoline per C
// callback slot and look up the Lua function at dispatch time using a
// registry-ref index stored in the trampoline's closure upvalue (for hooks)
// or in this map (for commands, which can't take user_data).

struct LuaCallback {
    int ref;                    // luaL_ref into LUA_REGISTRYINDEX
    std::string name;           // for diagnostics
};

static std::map<std::string, LuaCallback> g_atcmd_callbacks;
static std::map<std::string, LuaCallback> g_buildin_callbacks;

// Hooks: one entry per (hook_type, ref). Stored separately so we can clean up.
struct HookCallback {
    int hook_type;
    int ref;
};
static std::vector<HookCallback> g_hook_callbacks;

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

// ---- atcommand registration ----

static int32_t atcmd_dispatch(map_session_data* sd, const char* command,
                              const char* message) {
    auto it = g_atcmd_callbacks.find(command + 1); // skip '@' or '#'
    if (it == g_atcmd_callbacks.end()) return 0;
    lua_State* L = L_get();
    if (!L) return 0;

    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
    push_player(L, sd);
    lua_pushstring(L, message ? message : "");
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        wlog_warning("atcmd '%s' error: %s", it->first.c_str(),
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
static int lw_register_atcmd(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int level = (int)luaL_optinteger(L, 2, 0);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaCallback cb{ref, name};
    g_atcmd_callbacks[name] = cb;

    bool ok = g_api->atcmd.register_cmd(name, level, atcmd_dispatch);
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

struct BuildinCoro {
    script_state* st         = nullptr;
    lua_State*    T          = nullptr;
    int           thread_ref = LUA_NOREF; // keeps T alive against GC
    void*         token      = nullptr;   // null until first suspend
    std::string   name;
};

// Active coroutine for the currently-running Lua wrapper. Wrappers like
// sleep() consult this to know which script_state to suspend.
static thread_local BuildinCoro* g_active_coro = nullptr;

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
        // The yielding wrapper has already arranged a way to be resumed
        // (e.g. timer for sleep). Keep the coroutine alive — caller stays
        // suspended.
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

static int32_t buildin_dispatch_inner(script_state* st, const std::string& name,
                                      const std::string& spec) {
    auto it = g_buildin_callbacks.find(name);
    if (it == g_buildin_callbacks.end()) return PLUGIN_SCRIPT_CMD_FAILURE;
    lua_State* L = L_get();
    if (!L) return PLUGIN_SCRIPT_CMD_FAILURE;

    auto* coro = new BuildinCoro();
    coro->st   = st;
    coro->name = name;
    coro->T    = lua_newthread(L);
    coro->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Push the registered Lua function onto T's stack.
    lua_rawgeti(coro->T, LUA_REGISTRYINDEX, it->second.ref);

    map_session_data* sd = g_api->script.rid2sd(st);
    push_player(coro->T, sd);

    int nargs = 1; // player table
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

// Each registered buildin needs its own C function so plugin_api can route
// to the right Lua callback. We pre-allocate a small pool of trampolines.
//
// (Plugin API takes a function pointer with no closure data, so we can't
//  multiplex through a single dispatcher unless we know the command name.
//  These closures keep their name as a static so the dispatcher can look up
//  the matching callback.)

#define BUILDIN_TRAMPOLINE(N)                                            \
    static std::string g_buildin_name_##N;                               \
    static std::string g_buildin_spec_##N;                               \
    static int32_t buildin_dispatch_##N(script_state* st) {              \
        return buildin_dispatch_inner(st, g_buildin_name_##N,            \
                                      g_buildin_spec_##N);               \
    }

BUILDIN_TRAMPOLINE(0)  BUILDIN_TRAMPOLINE(1)  BUILDIN_TRAMPOLINE(2)
BUILDIN_TRAMPOLINE(3)  BUILDIN_TRAMPOLINE(4)  BUILDIN_TRAMPOLINE(5)
BUILDIN_TRAMPOLINE(6)  BUILDIN_TRAMPOLINE(7)  BUILDIN_TRAMPOLINE(8)
BUILDIN_TRAMPOLINE(9)  BUILDIN_TRAMPOLINE(10) BUILDIN_TRAMPOLINE(11)
BUILDIN_TRAMPOLINE(12) BUILDIN_TRAMPOLINE(13) BUILDIN_TRAMPOLINE(14)
BUILDIN_TRAMPOLINE(15) BUILDIN_TRAMPOLINE(16) BUILDIN_TRAMPOLINE(17)
BUILDIN_TRAMPOLINE(18) BUILDIN_TRAMPOLINE(19) BUILDIN_TRAMPOLINE(20)
BUILDIN_TRAMPOLINE(21) BUILDIN_TRAMPOLINE(22) BUILDIN_TRAMPOLINE(23)
BUILDIN_TRAMPOLINE(24) BUILDIN_TRAMPOLINE(25) BUILDIN_TRAMPOLINE(26)
BUILDIN_TRAMPOLINE(27) BUILDIN_TRAMPOLINE(28) BUILDIN_TRAMPOLINE(29)
BUILDIN_TRAMPOLINE(30) BUILDIN_TRAMPOLINE(31)
#undef BUILDIN_TRAMPOLINE

static plugin_script_func g_buildin_slots[] = {
    buildin_dispatch_0,  buildin_dispatch_1,  buildin_dispatch_2,
    buildin_dispatch_3,  buildin_dispatch_4,  buildin_dispatch_5,
    buildin_dispatch_6,  buildin_dispatch_7,  buildin_dispatch_8,
    buildin_dispatch_9,  buildin_dispatch_10, buildin_dispatch_11,
    buildin_dispatch_12, buildin_dispatch_13, buildin_dispatch_14,
    buildin_dispatch_15, buildin_dispatch_16, buildin_dispatch_17,
    buildin_dispatch_18, buildin_dispatch_19, buildin_dispatch_20,
    buildin_dispatch_21, buildin_dispatch_22, buildin_dispatch_23,
    buildin_dispatch_24, buildin_dispatch_25, buildin_dispatch_26,
    buildin_dispatch_27, buildin_dispatch_28, buildin_dispatch_29,
    buildin_dispatch_30, buildin_dispatch_31,
};

static std::string* const g_buildin_names[] = {
    &g_buildin_name_0,  &g_buildin_name_1,  &g_buildin_name_2,
    &g_buildin_name_3,  &g_buildin_name_4,  &g_buildin_name_5,
    &g_buildin_name_6,  &g_buildin_name_7,  &g_buildin_name_8,
    &g_buildin_name_9,  &g_buildin_name_10, &g_buildin_name_11,
    &g_buildin_name_12, &g_buildin_name_13, &g_buildin_name_14,
    &g_buildin_name_15, &g_buildin_name_16, &g_buildin_name_17,
    &g_buildin_name_18, &g_buildin_name_19, &g_buildin_name_20,
    &g_buildin_name_21, &g_buildin_name_22, &g_buildin_name_23,
    &g_buildin_name_24, &g_buildin_name_25, &g_buildin_name_26,
    &g_buildin_name_27, &g_buildin_name_28, &g_buildin_name_29,
    &g_buildin_name_30, &g_buildin_name_31,
};

static std::string* const g_buildin_specs[] = {
    &g_buildin_spec_0,  &g_buildin_spec_1,  &g_buildin_spec_2,
    &g_buildin_spec_3,  &g_buildin_spec_4,  &g_buildin_spec_5,
    &g_buildin_spec_6,  &g_buildin_spec_7,  &g_buildin_spec_8,
    &g_buildin_spec_9,  &g_buildin_spec_10, &g_buildin_spec_11,
    &g_buildin_spec_12, &g_buildin_spec_13, &g_buildin_spec_14,
    &g_buildin_spec_15, &g_buildin_spec_16, &g_buildin_spec_17,
    &g_buildin_spec_18, &g_buildin_spec_19, &g_buildin_spec_20,
    &g_buildin_spec_21, &g_buildin_spec_22, &g_buildin_spec_23,
    &g_buildin_spec_24, &g_buildin_spec_25, &g_buildin_spec_26,
    &g_buildin_spec_27, &g_buildin_spec_28, &g_buildin_spec_29,
    &g_buildin_spec_30, &g_buildin_spec_31,
};

static size_t g_buildin_used = 0;
static constexpr size_t BUILDIN_SLOT_MAX =
    sizeof(g_buildin_slots) / sizeof(g_buildin_slots[0]);

// register_buildin("name", "argspec", fn)
//   argspec follows rAthena conventions: 'i' = number, 's' = string.
//   fn(player, arg1, arg2, ...) is called when the command runs.
static int lw_register_buildin(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* spec = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (g_buildin_used >= BUILDIN_SLOT_MAX) {
        wlog_error("register_buildin: slot pool exhausted (%zu max)",
                   BUILDIN_SLOT_MAX);
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    size_t slot = g_buildin_used++;
    *g_buildin_names[slot] = name;
    *g_buildin_specs[slot] = spec;
    g_buildin_callbacks[name] = LuaCallback{ref, name};

    bool ok = g_api->script_addcommand(name, spec, g_buildin_slots[slot]);
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
        {"monster",             lw_monster},
        {"message",             lw_message},
        {"emotion",             lw_emotion},
        {"specialeffect",       lw_specialeffect},
        {"item_name",           lw_item_name},
        {"skill_name",          lw_skill_name},
        {"gettick",             lw_gettick},
        {"timer_after",         lw_timer_after},
        {"sleep",               lw_sleep},
        {"script_suspend",      lw_script_suspend},
        {"script_resume",       lw_script_resume},
        {"register_atcmd",      lw_register_atcmd},
        {"register_buildin",    lw_register_buildin},
        {"hook",                lw_hook},
        {nullptr, nullptr}
    };
    for (const luaL_Reg* r = fns; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
}

} // namespace workshop
