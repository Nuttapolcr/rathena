# script_eval / rathena() — run any rAthena script from Lua

**Area:** workshop / lua-api / script-bridge
**Last verified:** 2026-05-17 (commit plugin/workshop WIP)

`plugin_api_t.script.eval(const char* src, int32_t rid)` (impl
`api_script_eval` in `src/map/plugin.cpp`) compiles an arbitrary
rAthena script snippet and runs it synchronously. Lua surface:
`script_eval(src [, player])` and its alias `rathena(src [, player])`
(`lw_script_eval` in `plugins/workshop/lua_globals.cpp`).

This is the catch-all answer to "Lua must call every rAthena
subsystem". Rather than widening the typed `plugin_api_t` struct one
subsystem at a time (party/guild/mail/instance/clan/channel/bg/...),
`eval` runs through the script engine, so **every** buildin script
command is reachable immediately:

```lua
script_eval("getitem 501,1; mailbox;", player)   -- player gets the item
rathena([[
    setarray .@m$[0], "prontera","geffen";
    announce "Event starting in "+ .@m$[0], bc_all;
]])                                               -- no player → rid 0
```

## How it works

- `parse_script(src, "plugin_eval", 0, SCRIPT_IGNORE_EXTERNAL_BRACKETS)`
  → `struct script_code*` (script.hpp:2308-2309). The
  `SCRIPT_IGNORE_EXTERNAL_BRACKETS` option means the source does not
  need wrapping `{ }`.
- `run_script(code, 0, rid, fake_nd->id)` (script.cpp:4241). `fake_nd`
  (npc.hpp:1689, the engine's ownerless NPC used for item scripts) is
  the execution vehicle — no real NPC needed.
- `rid` is the player **account id**. Lua side resolves the optional
  `player` arg via `sd_from_arg` then `g_api->pc.get_aid(sd)`. With
  rid set, `rid2sd`/`getcharid`/`getitem`/etc. target that player.
- `script_free_code(code)` frees the transient code after the run.

## Why not widen the typed API instead

Hybrid by design (user decision): `eval` is the primary
"call-anything" path; the typed sub-structs (`pc`, `mob`, `status`,
…) stay for hot-path / type-safe access. New typed accessors are
added only when profiling shows an `eval` path is too slow — not
preemptively.

## Constraints / hazards

- **No suspension.** The snippet must complete synchronously. `sleep`,
  `sleep2`, and dialog primitives (`mes`+`next`/`menu`/`input`) park
  the `script_state`; that parked state still references the code we
  free here → use-after-free. v1 contract forbids suspending commands
  in `eval`. For sleeping work use a real `register_buildin` handler
  (which has `script_suspend`/`sleep` support) instead.
- **Full script authority.** `eval` runs with the same power as any
  NPC script (atcommand, item grants, var writes, `nuke`-equivalents).
  Mods are trusted code; treat `script_eval` input as privileged —
  never build a snippet from unsanitised client/player input.
- **Parse cost.** Every call re-parses the source. For a hot path,
  cache results in Lua or use a typed accessor; do not `eval` per
  packet/frame.
- **Reload-safe.** `eval` holds no Lua registry ref and no engine
  registration, so `workshop_reload` needs no special handling for it.

## ABI

`eval` is the **last** member of `plugin_script_api_t` (appended after
`get_var_str`); the function pointer is the last entry in the script
sub-struct initializer in `src/map/plugin.cpp`. Engine and plugin
build from one tree, so this is consistent; the append-at-end rule
keeps the diff reviewable.
