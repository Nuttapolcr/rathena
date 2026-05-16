# Workshop client-packet API (decode + encode + custom ids)

**Area:** workshop / lua-api / packets
**Last verified:** 2026-05-17 (commit plugin/workshop WIP)

Full client-packet surface for Lua mods. Code: `plugins/workshop/
lua_globals.cpp` (`lw_on_packet`, `lw_register_packet`,
`lw_packet_read_*`, `lw_packet_send*`, the `PacketWriter` block, and
the `pw_methods[]` metatable). C ABI: `plugin_packet_api_t` in
`src/map/plugin.hpp`.

## Decode (incoming)

- `on_packet(cmd, fn)` — filter that runs *before* the engine clif
  handler for an existing cmd. `fn(ctx)`, `ctx = {fd, cmd, player?}`.
  Return `false`/`"stop"` → `PLUGIN_PACKET_STOP` (engine handler
  skipped). nil/true/number → pass through. Engine side:
  `g_api->packet.register_filter`, trampoline
  `packet_filter_trampoline`.
- `register_packet(cmd, length, fn)` — claim a previously-unused cmd
  (0x064..0xCFF). `length` = fixed byte size incl. 2-byte cmd, or `-1`
  for variable-length (size is a uint16 at offset 2). Engine side:
  `g_api->packet.register_handler`.
- Read helpers (call with `ctx.fd`): `packet_read_b/w/l(fd, off)`
  (1/2/4-byte little-endian), `packet_read_str(fd, off)` (pointer into
  recv buffer), `packet_rest(fd)` (bytes left). `off` is from the cmd
  word (cmd at 0..1).

## Encode (outgoing) — `packet_writer()`

Returns a `rathena.PacketWriter` userdata. All `write_*` are
little-endian and return self for chaining.

| method | effect |
|---|---|
| `write_b/w/l/q(v)` | append 1/2/4/8-byte LE int |
| `write_str(s [,n])` | n given → fixed n-byte field, `\0`-padded/truncated; else raw bytes, no terminator |
| `write_bytes(s)` | append raw Lua-string bytes |
| `write_zeros(n)` | append n `0x00` |
| `set_b/w/l(off,v)` | patch in place (bounds-checked no-op if OOB) — for variable-length size word |
| `len()` / `#w` | current byte count |
| `build()` | return buffer as a Lua string |

`build()` does **not** prepend the cmd — write the cmd word yourself
at offset 0. Variable-length pattern: write a placeholder size word,
fill body, `set_w(2, w:len())`.

## Send

- `packet_send_self(fd, bytes)` — push raw packet to one fd
  (`clif`-style direct).
- `packet_send(player|nil, bytes [, target])` — `clif_send`; `target`
  is `enum send_target` (0=ALL_CLIENT, 1=ALL_SAMEMAP, 2=AREA,
  3=AREA_WOS, 24=SELF, …). `player` may be nil only for target 0.

## Lifetime / reload

- One filter per cmd and one handler per cmd; re-registering swaps the
  bound function (the old Lua ref is `luaL_unref`'d).
- On `workshop_reload`: `stop_event_system` unrefs and clears
  `g_packet_filter_refs` / `g_packet_handler_refs`, but the engine
  **keeps** the filter/handler registered with a now-stale ref as
  user_data. The trampolines no-op on that (`lua_rawgeti` →
  not-a-function guard), and a re-run of the mod re-registers cleanly.
  So packet hooks survive reload without an engine-side unregister.
- `PacketWriter` is a pure Lua object (a `std::vector<uint8_t>` in
  userdata, freed by `lw_pw_gc`); it holds no engine ref and is
  inherently reload-safe.

## Gotchas

- Offsets are always from the cmd word (offset 0), not from the
  payload start.
- `packet_read_str` returns a pointer into the live recv buffer — copy
  it into a Lua string immediately; don't stash the value across
  yields.
- Picking a custom cmd that the client build actually uses will
  shadow the engine handler (via `register_filter` semantics) or be
  ignored — use an id you've confirmed is free.
