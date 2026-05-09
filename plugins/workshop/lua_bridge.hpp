#ifndef WORKSHOP_LUA_BRIDGE_HPP
#define WORKSHOP_LUA_BRIDGE_HPP

#include <string>

struct lua_State;

namespace workshop {

// Owns the global Lua VM that all mods share. One VM keeps cross-mod calls
// cheap; mods get isolated environments via _ENV when we evaluate their files.
class LuaBridge {
public:
    static LuaBridge& instance();

    bool init();
    void shutdown();

    lua_State* L() const { return L_; }

    // Run a Lua chunk from disk. Auto-detects bytecode (luac) vs source —
    // `luaL_loadfilex` already handles both via the first byte.
    bool run_file(const std::string& path, const std::string& chunk_name = {});

    // Pcall a global function by name with no args (for on_init hooks).
    bool call_global(const char* name);

    // Last error string after a failed run_file/call_global.
    const std::string& last_error() const { return last_err_; }

private:
    LuaBridge() = default;
    LuaBridge(const LuaBridge&) = delete;

    lua_State* L_ = nullptr;
    std::string last_err_;
};

// Register all rAthena script wrappers as Lua globals on `L`.
void register_globals(lua_State* L);

} // namespace workshop

#endif
