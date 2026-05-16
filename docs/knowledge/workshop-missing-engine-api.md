# Storage/UI/intif plugin API: Lua side shipped without engine side

**Area:** plugin-loader / workshop
**Last verified:** 2026-05-17 (commit plugin/workshop WIP)

Commit `edc1245b6` ("workshop: Lua wrappers for storage/UI plugin API +
intif_connected hook") on branch `plugin/workshop` added the **Lua
half** in `plugins/workshop/lua_globals.cpp` (`open_guild_storage`,
`open_storage2`, `storage_exists`, `register_storage`, `storage_name`,
`open_ui`, `open_dressroom`, `open_roulette`, `open_mail`, and
`hook("intif_connected"|"char_reconnect")`) but the matching
**engine half** never landed on this branch.

Symptoms: `make -C plugins/workshop` failed with `struct plugin_api_t
has no member named 'ui'`, missing `storage.open_guild` /
`open_premium` / `exists` / `define` / `get_name`, and
`HOOK_INTIF_CONNECTED` / `plugin_intif_connected_t` not declared. The
prebuilt `workshop.so` predated the breaking commit, hiding it.

The engine half exists as commit `a3a9c7a6f` ("Expose storage variants,
client UI windows, and char-(re)connect hook to plugins", branch
`plugin-system`): adds `plugin_ui_api_t` (`ui.open` →
`clif_ui_open`, `ui.dressroom`, `ui.roulette`, `ui.mail`), extends
`plugin_storage_api_t` (`open_guild`, `open_premium`, `exists`,
`define`, `get_name`), and adds `HOOK_INTIF_CONNECTED` +
`plugin_intif_connected_t { bool first; }` fired from `chrif_on_ready`
(`src/map/chrif.cpp`). It was cherry-picked onto `plugin/workshop` to
restore the build.

**Lesson:** a workshop Lua-API commit and its `src/map/plugin.{hpp,cpp}`
engine commit must land together. After pulling/branch-switching, run
`make -C plugins/workshop` AND `make map`; a stale `workshop.so` will
mask a `plugin_api_t` ABI mismatch (the auto-generated header deps in
`plugins/workshop/Makefile` only force a rebuild when you actually
recompile).
