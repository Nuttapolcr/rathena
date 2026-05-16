# on_event / npc_event_all — rAthena script_config labels in Lua

**Area:** workshop / lua-api / npc-events
**Last verified:** 2026-05-17 (commit plugin/workshop WIP)

`on_event(label, fn)` binds a Lua handler to **any** rAthena NPC-script
event label — the same labels NPC `.txt` scripts use (`OnInit`,
`OnPCLoginEvent`, `OnClock1300`, ...). `npc_event_all(label [,player])`
broadcasts a label to every NPC that defines it (the `donpcevent`
mechanism). Code: `plugins/workshop/lua_globals.cpp`
(`fire_event`, `npc_script_event_dispatch`, `npc_event_doall_dispatch`,
`lw_on_event`, `lw_npc_event_all`); engine side `src/map/npc.cpp`,
`src/map/plugin.cpp`, `src/map/plugin.hpp`.

## Two engine funnels → one Lua registry

| Hook | Engine fire site | Covers | ctx.player |
|---|---|---|---|
| `HOOK_NPC_SCRIPT_EVENT` | `npc_script_event()` src/map/npc.cpp:5831 | PC-attached `script_config` labels | yes |
| `HOOK_NPC_EVENT_DOALL` | `npc_event_doall_id()` src/map/npc.cpp:1334 | broadcast labels: OnInit, OnInterIfInit, OnAgit*, clock labels, donpcevent | no |

`npc_event_doall()` delegates to `npc_event_doall_id(name, 0)`, so the
single fire site in `_id` covers both. Both hooks call into
`fire_event(label[, sd])`, which invokes every handler bound via
`on_event(label, fn)` as `fn(label, ctx)` — `ctx = { event=label,
player? }`. Existing handlers that only read the first arg keep working
(extra args are ignored in Lua).

## script_config PC label map (`npc_get_script_event_name`, npc.cpp:5984)

| npce_event | default label (script.cpp:258-300) |
|---|---|
| NPCE_LOGIN | OnPCLoginEvent |
| NPCE_LOGOUT | OnPCLogoutEvent |
| NPCE_LOADMAP | OnPCLoadMapEvent |
| NPCE_BASELVUP | OnPCBaseLvUpEvent |
| NPCE_JOBLVUP | OnPCJobLvUpEvent |
| NPCE_DIE | OnPCDieEvent |
| NPCE_KILLPC | OnPCKillEvent |
| NPCE_KILLNPC | OnNPCKillEvent |
| NPCE_IDENTIFY | OnPCIdentifyEvent |

`ctx.npce_type` is the enum value; `ctx.label` (in the C struct) is the
resolved name. If a server renames a label in `conf/script.conf`, the
resolved name follows automatically (we pass
`npc_get_script_event_name(type)`, not a hardcoded string).

```lua
on_event("OnPCLoginEvent", function(label, ctx)
    if ctx.player then message(ctx.player, "welcome back") end
end)
on_event("OnInit", function() log_info("all NPCs loaded") end)
on_event("OnClock0000", function() announce("midnight") end)   -- engine clock
npc_event_all("OnMyCustom")                                     -- broadcast
```

## Clock dedup — behaviour change

The workshop previously ran its **own** 1-second wall-clock ticker
(`clock_tick_cb` + `add_timer_interval`) that synthesised
OnClock/OnMinute/OnHour/OnDay/OnSun..Sat. That ticker is **removed**.
Clock labels now come solely from the engine's `npc_event_do_clock` →
`npc_event_doall` → `HOOK_NPC_EVENT_DOALL` path. Consequences:

- Clock labels fire **once** and in lockstep with NPC scripts (no
  double-fire, no drift between Lua and `.txt` clock handlers).
- `on_clock`/`on_minute`/`on_hour`/`on_day` still work — they only
  synthesise the same label strings into `g_event_handlers`.
- A Lua-only deployment with **no NPC clock infrastructure** still gets
  clock events, because the engine's clock dispatcher runs regardless
  of whether any NPC subscribes.

## Cancel semantics

Both hooks are **informational** — returning `false`/`"stop"` from an
`on_event` handler does nothing. The label is already being broadcast
to NPCs by the time the hook fires. To gate an action, use the
relevant cancellable `hook()` event instead (e.g. `pc_dead`,
`pc_login` is not cancellable — see workshop-hook-context-map.md).

## Reload / lifetime

- The two engine hooks are registered **once per process**
  (`s_npc_event_hooks_added` static in `start_event_system`). The
  engine keeps hook registrations across `workshop_reload`; their C
  dispatcher and null user_data stay valid, so re-adding on reload
  would double-fire — hence the static guard.
- `fire_event` early-returns when no handler is bound for the label
  and when `L_get()` is null, so the very frequent
  `npc_event_doall` traffic (every clock minute + every `donpcevent`)
  is O(1) when idle.
- `g_event_handlers` refs are unref'd/cleared in `stop_event_system`;
  a reload re-runs mods and re-registers handlers cleanly.

## ABI

`HOOK_NPC_SCRIPT_EVENT` / `HOOK_NPC_EVENT_DOALL` are inserted **before
`HOOK_MAX`** (no existing enum value shifts). `npc.event_all` /
`npc.event_all_rid` are appended at the end of `plugin_npc_api_t`.
Engine + plugin build from one tree.
