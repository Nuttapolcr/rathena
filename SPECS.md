# SPECS.md

This file documents features added to the rAthena project. Each feature entry should include:
- **Feature name** — what was added
- **Date** — when it was added (YYYY-MM-DD)
- **Description** — what the feature does and why it was added
- **Files changed** — which files were modified or created
- **Related commits** — git commit hashes or branch references
- **Research/Knowledge** — key technical findings, architecture decisions, APIs discovered, and how-to guidance that can inform future work (e.g., "packet bytes are little-endian", "plugin API uses function pointers in `plugin_api_t` struct", "Lua userdata needs explicit `__gc` metamethod for C++ cleanup")

## Features

### Plugin Workshop: Lua API & Hooks (2026-05-14)
- **Description:** Expanded Lua plugin API with storage/UI wrappers, battle config access (get/set/has), and client packet hooks (on_packet / register_packet). Added intif_connected hook for inter-server communication events.
- **Files:** src/map/plugin.hpp, plugins/workshop/
- **Related commits:** edc1245b6
- **Research/Knowledge:**
  - **Hook system:** Hooks are dispatched from inside gameplay code via `plugin_hook_fire(HOOK_*, &payload)`. Returning `HOOK_STOP` vetoes the event (for pre-action hooks); informational hooks ignore return values. Hooks stored in plugin-side `g_hook_callbacks` vector with `luaL_ref` to keep Lua callbacks alive.
  - **Packet hooks:** Two modes: `register_filter()` intercepts existing packet IDs before engine handler (can suppress with `PLUGIN_PACKET_STOP`); `register_handler()` claims unused cmd IDs in `packet_db`. Both receive fd, map_session_data*, and plugin user_data.
  - **Storage/UI APIs:** Exposed via nested structs in `plugin_api_t`. Storage has get/set methods for custom data stores. UI typically uses packet sends (e.g., `clif_send()`) under the hood.
  - **Battle config:** Stored in global `battle_config` map; accessed via C-style config name strings. Plugin API exposes get/set/has for runtime tuning without game restart.
  - **Inter-server communication:** `intif_connected` hook fires when char-server reconnects; useful for resynchronizing state or reloading guild/party data.

### Lua PacketWriter API (2026-05-14)
- **Description:** Added PacketWriter userdata type for Lua scripts to construct custom binary packets field-by-field, then send them to players via the existing `packet_send()` / `packet_send_self()` functions. Provides methods for writing/patching bytes, words, dwords, qwords, and strings with little-endian encoding. Enables Lua mods to implement custom client protocols without manual binary string manipulation.
- **Files:** plugins/workshop/lua_globals.cpp
- **Related commits:** 8b78a0c6b
- **Usage:** 
  ```lua
  local w = packet_writer()
  w:write_w(0x007F)      -- opcode (2 bytes LE)
  w:write_l(tick)        -- data (4 bytes LE)
  w:write_str("Hello!")  -- string bytes
  w:set_w(0, w:len())    -- patch total length at offset 0
  packet_send(player, w:build())  -- send to player
  ```
- **API Methods:** write_b/w/l/q, write_str, write_bytes, write_zeros, set_b/w/l, len, build, __gc, __len
- **Research/Knowledge:**
  - **Packet structure:** All map-server packets are little-endian binary. Fixed-size packets use stack structs; variable-length use the global `packet_buffer[UINT16_MAX]` in `src/common/socket.cpp`.
  - **Plugin API:** Functions are exposed via `plugin_api_t` struct (defined in `src/map/plugin.hpp`) with function pointers. Each sub-API (clif, packet, pc, etc.) is a nested struct within `plugin_api_t`.
  - **Lua userdata:** Created via `lua_newuserdata()` + placement new for C++ objects. Requires `__gc` metamethod to call destructor; without it, `std::vector` would leak. Metatable registered with `luaL_newmetatable()`, methods exposed via `__index` table.
  - **Sending packets:** `packet_send_target(bl, data, len, target)` routes via `clif_send()` which loops over target set and writes to each fd via `WFIFOHEAD/memcpy/WFIFOSET` macros (defined in `src/common/socket.hpp`). Direct send via `packet_send_self(fd, data, len)`.
  - **Lua method binding:** Methods registered as `luaL_Reg` array in metatable `__index`. Method functions take `lua_State*` and return count of pushed return values. `lua_pushvalue(L, 1)` enables method chaining.
  - **Little-endian encoding:** Used throughout rAthena networking. Multiple-byte integers written LSB-first: byte 0 = `val & 0xFF`, byte 1 = `(val >> 8) & 0xFF`, etc. Helper `write_le()` automates this.

