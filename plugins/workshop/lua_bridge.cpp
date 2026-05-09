#include "lua_bridge.hpp"
#include "workshop.hpp"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

namespace workshop {

LuaBridge& LuaBridge::instance() {
    static LuaBridge inst;
    return inst;
}

// Print() override — route Lua print() into the server log so mod output
// shows up alongside other server messages.
static int lua_print_to_log(lua_State* L) {
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addlstring(&b, s, len);
        lua_pop(L, 1);
    }
    luaL_pushresult(&b);
    const char* msg = lua_tostring(L, -1);
    wlog_info("lua: %s", msg ? msg : "");
    lua_pop(L, 1);
    return 0;
}

bool LuaBridge::init() {
    if (L_) return true;
    L_ = luaL_newstate();
    if (!L_) {
        last_err_ = "luaL_newstate returned null";
        return false;
    }
    luaL_openlibs(L_);

    lua_pushcfunction(L_, lua_print_to_log);
    lua_setglobal(L_, "print");

    return true;
}

void LuaBridge::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

bool LuaBridge::run_file(const std::string& path, const std::string& chunk_name) {
    if (!L_) {
        last_err_ = "Lua VM not initialised";
        return false;
    }

    // luaL_loadfile transparently handles both Lua source and pre-compiled
    // bytecode (it sniffs the leading byte: 0x1B == LUA_SIGNATURE).
    const char* name = chunk_name.empty() ? path.c_str() : chunk_name.c_str();
    int rc = luaL_loadfilex(L_, path.c_str(), nullptr);
    if (rc != LUA_OK) {
        last_err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "load failed";
        lua_pop(L_, 1);
        return false;
    }
    (void)name;

    rc = lua_pcall(L_, 0, 0, 0);
    if (rc != LUA_OK) {
        last_err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "runtime error";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaBridge::call_global(const char* name) {
    if (!L_) return false;
    lua_getglobal(L_, name);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return true; // not defined — treat as no-op
    }
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        last_err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "call failed";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace workshop
