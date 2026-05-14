# SPECS.md

This file documents features added to the rAthena project. Each feature entry should include:
- **Feature name** — what was added
- **Date** — when it was added
- **Description** — what the feature does and why it was added
- **Files changed** — which files were modified or created
- **Related commits** — git commit hashes or branch references

## Features

### Plugin Workshop: Lua API & Hooks (2026-05-14)
- **Description:** Expanded Lua plugin API with storage/UI wrappers, battle config access (get/set/has), and client packet hooks (on_packet / register_packet). Added intif_connected hook for inter-server communication events.
- **Files:** src/map/plugin.hpp, plugins/workshop/
- **Related commits:** edc1245b6

### Lua PacketWriter API (2026-05-14)
- **Description:** Added PacketWriter userdata type for Lua scripts to construct custom binary packets field-by-field, then send them to players via the existing `packet_send()` / `packet_send_self()` functions. Provides methods for writing/patching bytes, words, dwords, qwords, and strings with little-endian encoding. Enables Lua mods to implement custom client protocols without manual binary string manipulation.
- **Files:** plugins/workshop/lua_globals.cpp
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

