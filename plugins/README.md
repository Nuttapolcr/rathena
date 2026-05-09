# rAthena Plugin System

> **Languages:** English · [ภาษาไทย](README.th.md)

A dynamic plugin system that lets external DLL / SO files hook into
map-server events, register custom script commands, custom `@commands`,
and custom client packets — without modifying the rAthena source.

---

## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Building a Plugin](#building-a-plugin)
- [The Plugin Lifecycle](#the-plugin-lifecycle)
- [Hooks](#hooks)
- [The API Surface](#the-api-surface)
  - [`script` — Script state helpers](#script--script-state-helpers)
  - [`pc` — Player operations](#pc--player-operations)
  - [`mob` — Monsters](#mob--monsters)
  - [`map` — Map / iteration](#map--map--iteration)
  - [`status` — HP / SP](#status--hp--sp)
  - [`bl` — Block list](#bl--block-list)
  - [`item_api` — Items & item DB](#item_api--items--item-db)
  - [`atcmd` — Custom @commands](#atcmd--custom-commands)
  - [`quest` — Quests](#quest--quests)
  - [`npc` — NPC events](#npc--npc-events)
  - [`skill` — Skills](#skill--skills)
  - [`storage` — Storage](#storage--storage)
  - [`clif` — Client packets](#clif--client-packets)
  - [`timer` — Timers](#timer--timers)
  - [`log` — Logging](#log--logging)
  - [`packet` — Custom packets](#packet--custom-packets)
- [Patterns](#patterns)
- [Example Plugin](#example-plugin)
- [Tips & Troubleshooting](#tips--troubleshooting)

---

## Overview

A plugin is a shared library (`.so` on Linux/macOS, `.dll` on Windows)
that exports three C-linkage symbols. The map-server loads each plugin
listed in `conf/plugins.conf` at startup, hands it an
[`plugin_api_t`](../src/map/plugin.hpp) struct full of function
pointers, and lets the plugin install hook callbacks or new script /
@commands / packet handlers.

**Why a plugin instead of a custom build?**

- Reload only the plugin to iterate on logic — no full server rebuild.
- Distribute features as drop-in `.so` / `.dll` files.
- Keep server upgrades clean — your custom code lives outside `src/`.

The plugin surface is intentionally narrow: plugins include only
`map/plugin.hpp`, never the rest of the server headers. All server
types (`map_session_data`, `mob_data`, `block_list`, …) are forward-
declared and accessed through accessor functions on the API struct.
This means the plugin ABI does not depend on field layouts inside
the server.

---

## Quick Start

1. **Enable a plugin** by adding its path to `conf/plugins.conf`:

   ```text
   plugins/example/example_plugin
   ```

   The platform extension (`.so` / `.dll`) is appended automatically.

2. **Build** the example:

   ```bash
   make plugin
   ```

   This produces `plugins/example/example.so` (Linux) or
   `example_plugin.dll` (Windows).

3. **Start the server**. You should see:

   ```text
   [Status]: plugin: Loaded 'Example Plugin' v3.0.0 by rAthena Dev Team
   [Status]: plugin: 1 plugin(s) loaded.
   ```

4. **Try it** in any NPC script:

   ```c
   prontera,150,150,4    script    PluginTest    4_F_KAFRA1,{
       mes "Hello, plugin!";
       plugin_announce "Server-wide message from a plugin!";
       .@n = plugin_async_roll(6);
       mes "You rolled: " + .@n;
       close;
   }
   ```

---

## Building a Plugin

A plugin is a single C++ file (or several) that includes
`map/plugin.hpp` and exports three symbols:

```cpp
PLUGIN_API plugin_info_t* plugin_info();
PLUGIN_API bool           plugin_init(plugin_api_t* api);
PLUGIN_API void           plugin_final();
```

### Linux / macOS

The supplied `make plugin` target builds every directory under
`plugins/`. Add your plugin as a sibling of `example/`:

```text
plugins/
├── example/
│   └── example_plugin.cpp
└── my_feature/
    └── my_feature.cpp     ← discovered automatically
```

`make plugin` invokes `g++ -std=c++17 -shared -fPIC -fvisibility=hidden`
on every `.cpp` in each plugin directory and produces a `.so` named
after the directory.

### Windows (Visual Studio)

Use [`plugins/example/example_plugin.vcxproj`](example/example_plugin.vcxproj)
as a template. The important compiler / linker settings are:

- **Configuration Type:** Dynamic Library (`.dll`)
- **Additional Include Directories:** `<rathena>/src`
- **Output Name:** match what you list in `conf/plugins.conf`

### CMake (cross-platform)

The shared [`plugins/CMakeLists.txt`](CMakeLists.txt) walks every
sibling directory and builds each one. Standalone use:

```bash
cd plugins
mkdir build && cd build
cmake -DRATHENA_SRC=../../src ..
cmake --build .
```

---

## The Plugin Lifecycle

```text
        ┌──────────────┐
        │ map-server   │
        │ starts       │
        └──────┬───────┘
               │
               ▼
   reads conf/plugins.conf
               │
               ▼
   for each path:
       dlopen / LoadLibrary
       call plugin_info()       ← returns metadata
       call plugin_init(api)    ← register hooks / commands
                │
                ▼
       ┌──── server runs ────┐
       │                     │
       │   hooks fire        │
       │   commands invoked  │
       │   packets dispatch  │
       │                     │
       └─────────────────────┘
                │
                ▼
        server shuts down
                │
                ▼
   for each loaded plugin:
       call plugin_final()      ← release any plugin-owned state
       dlclose / FreeLibrary
```

Before closing each DLL the manager:

- Clears every plugin-registered `@command` so post-shutdown lookups
  do not call into freed memory.
- Clears every plugin-registered packet handler in `packet_db`.
- Drops every parked script-state token so a stray `resume()` is a
  safe no-op.

You generally only need to remove your hooks in `plugin_final()`.

### Required exports

```cpp
static plugin_info_t info = {
    "My Plugin",                 // name
    "Author",                    // author
    "1.0.0",                     // version
    "What this plugin does"      // description
};

PLUGIN_API plugin_info_t* plugin_info() { return &info; }

static plugin_api_t* g_api = nullptr;

PLUGIN_API bool plugin_init(plugin_api_t* api) {
    g_api = api;
    // … register hooks, commands, packets …
    return true;
}

PLUGIN_API void plugin_final() {
    // … remove hooks if you registered any …
}
```

---

## Hooks

Hooks let your plugin observe — and optionally veto — gameplay events.

### Subscribing

```cpp
api->hook_add(HOOK_PC_LOGIN, on_login, /*user_data=*/nullptr, /*priority=*/100);
```

Lower priority runs first. Multiple plugins can subscribe to the same
hook; they fire in priority order.

### Callback signature

```cpp
static int on_login(void* data, void* user_data) {
    auto* d = static_cast<plugin_pc_login_t*>(data);
    // … use d->sd …
    return HOOK_CONTINUE;        // or HOOK_STOP to veto the event
}
```

`HOOK_STOP` is honoured by hooks that fire *before* an action, e.g.
`HOOK_PC_DEAD`, `HOOK_ITEM_USE`, `HOOK_TRADE_REQUEST`. Informational
hooks like `HOOK_PC_BASELEVELUP` ignore the return.

### Available hook points

| Category   | Hook                                                           | Veto? |
|-----------|----------------------------------------------------------------|:----:|
| Mob       | `HOOK_MOB_KILL`, `HOOK_MOB_SPAWN`                              |  ·   |
| Player    | `HOOK_PC_LOGIN`, `HOOK_PC_LOGOUT`                              |  ·   |
|           | `HOOK_PC_BASELEVELUP`, `HOOK_PC_JOBLEVELUP`                    |  ·   |
|           | `HOOK_PC_DEAD`                                                 |  ✓   |
| Inventory | `HOOK_ITEM_USE`, `HOOK_ITEM_PICKUP`, `HOOK_ITEM_DROP`, `HOOK_ITEM_EQUIP` |  ✓   |
| Skills    | `HOOK_SKILL_USE`                                               |  ✓   |
| Chat      | `HOOK_PC_CHAT`, `HOOK_PC_WHISPER`, `HOOK_PC_PARTYCHAT`, `HOOK_PC_GUILDCHAT` |  ✓   |
| NPC       | `HOOK_NPC_CLICK`                                               |  ✓   |
| Trade     | `HOOK_TRADE_REQUEST`, `HOOK_TRADE_COMMIT`                      |  ✓   |
| Party     | `HOOK_PARTY_CREATE`, `HOOK_PARTY_LEAVE`                        |  ✓   |
| Guild     | `HOOK_GUILD_CREATE`, `HOOK_GUILD_JOIN`, `HOOK_GUILD_LEAVE`     |  ✓*  |
| Status    | `HOOK_STATUS_CHANGE_START`, `HOOK_STATUS_CHANGE_END`           |  ✓*  |
| Vending   | `HOOK_VENDING_OPEN`, `HOOK_VENDING_BUY`                        |  ✓   |
| Storage   | `HOOK_STORAGE_OPEN`                                            |  ✓   |
| Quests    | `HOOK_QUEST_ADD`, `HOOK_QUEST_COMPLETE`                        |  ✓*  |
| Companion | `HOOK_PET_BORN`, `HOOK_PET_CATCH`, `HOOK_HOMUN_CALL`, `HOOK_HOMUN_LEVELUP` |  ✓* |
| Commands  | `HOOK_ATCMD_EXECUTE`                                           |  ·   |

*✓\* = veto for the leading event in the row only; see
[`plugin.hpp`](../src/map/plugin.hpp) for the data struct attached to
each hook and the exact semantics.*

---

## The API Surface

Every sub-struct on `plugin_api_t` groups related operations. The full
declaration lives in [`src/map/plugin.hpp`](../src/map/plugin.hpp).

### `script` — Script state helpers

Use inside a custom script command (`script_addcommand`).

| Function | Purpose |
|---|---|
| `hasdata(st, n)` | Is argument `n` present? |
| `getnum(st, n)` | Read argument `n` as integer |
| `getstr(st, n)` | Read argument `n` as string |
| `pushint(st, v)` / `pushstr(st, s)` | Push the return value |
| `rid2sd(st)` | Get the attached player session, or `nullptr` |
| `suspend(st)` | Park the script; return an opaque token. *See [Async script commands](#async-script-commands).* |
| `resume(token)` | Continue a parked script. No-op on stale tokens. |

### `pc` — Player operations

| Function | Purpose |
|---|---|
| `message(fd, msg)` | Send a chat-window line to one player |
| `additem(sd, &it, amount, log_type)` | Give items |
| `delitem(sd, idx, amount, type, reason, log_type)` | Take items |
| `gainexp(sd, src, base, job, flag)` | Grant EXP |
| `payzeny(sd, z, log_type)` / `getzeny(sd, z, log_type)` | Zeny in/out |
| `setpos(sd, mapindex, x, y, clrtype)` | Warp |
| `get_fd / get_aid / get_name` | Session lookup |
| `get_blv / get_jlv / get_mapid / get_pos_x / get_pos_y` | Stats lookup |
| `as_bl(sd)` | Cast to `block_list*` |

### `mob` — Monsters

| Function | Purpose |
|---|---|
| `once_spawn(...)` | Spawn N mobs at a position |
| `get_id / get_x / get_y / get_name` | Mob accessors |

### `map` — Map / iteration

| Function | Purpose |
|---|---|
| `name2id(name)` / `id2name(idx)` | Map index ↔ name |
| `id2sd(id)` / `charid2sd(cid)` / `nick2sd(name, allow_partial)` | Find session |
| `foreachinmap(cb, user, m, type_mask)` | Iterate every block-list of given types on map `m` |
| `foreachinarea(cb, user, m, x0, y0, x1, y1, type_mask)` | Same, but in a rectangle |
| `get_mapflag(m, flag)` | Read an `e_mapflag` |

`type_mask` is a bitmask: `1 << PLUGIN_BL_PC` for players,
`1 << PLUGIN_BL_MOB` for monsters, etc.

### `status` — HP / SP

| Function | Purpose |
|---|---|
| `heal(bl, hp, sp, flag)` | Restore HP/SP |
| `damage(src, target, hp, sp, walkdelay, flag, skill_id)` | Apply damage |

### `bl` — Block list

| Function | Purpose |
|---|---|
| `get_type(bl)` | Compare against `e_plugin_bl_type` (`PLUGIN_BL_PC`, `PLUGIN_BL_MOB`, …) |
| `as_sd(bl)` | Cast to `map_session_data*` if it is a player, else `nullptr` |

### `item_api` — Items & item DB

| Function | Purpose |
|---|---|
| `get_nameid(item*)` | Read nameid from an `item` struct |
| `db_exists(nameid)` | Does this id exist? |
| `db_get_name(nameid)` | Internal name (`Apple`) |
| `db_get_ename(nameid)` | Display name (`Apple`) |
| `db_get_type(nameid)` | `IT_HEALING`, `IT_USABLE`, … |

### `atcmd` — Custom @commands

```cpp
api->atcmd.register_cmd("myhello", /*level=*/0, on_atcmd_myhello, /*user_data=*/nullptr);
```

`level` is the minimum group ID required to run the command. The
`user_data` pointer is passed back unchanged on every dispatch — see
[Closure data](#closure-data-user_data).

### `quest` — Quests

| Function | Purpose |
|---|---|
| `add(sd, quest_id)` | Add to player's log |
| `update_status(sd, quest_id, state)` | `Q_INACTIVE / Q_ACTIVE / Q_COMPLETE` |
| `check(sd, quest_id, type)` | `HAVEQUEST / PLAYTIME / HUNTING` |

### `npc` — NPC events

| Function | Purpose |
|---|---|
| `event(sd, "NpcExname::OnLabel", ontouch)` | Trigger an NPC event label |

### `skill` — Skills

| Function | Purpose |
|---|---|
| `get_lv(sd, skill_id)` | Player's level in a skill |
| `use_id(sd, skill_id, lv, target_id)` | Cast a skill on a target |
| `get_name(skill_id)` | AEGIS name (`MG_FIREBOLT`) |
| `get_inf(skill_id)` | `INF_*` flags |
| `name2id(name)` | Reverse lookup |

### `storage` — Storage

| Function | Purpose |
|---|---|
| `open(sd)` | Open the player's personal storage |

### `clif` — Client packets

Pre-built sends for common client-facing effects.

| Function | Purpose |
|---|---|
| `displaymessage(fd, msg)` | One chat line |
| `emotion(bl, emote)` | Emote bubble (see `emotion_type`) |
| `specialeffect(bl, id, target)` / `specialeffect_single(bl, id, fd)` | Visual effect |
| `progressbar(sd, color, seconds)` / `progressbar_abort(sd)` | Cast bar |
| `broadcast(bl, msg, type, target)` | Server / map announcements |
| `messagecolor(bl, color, msg, rgb2bgr, target)` | Coloured chat |

`color` values are `0xRRGGBB`. `target` matches `enum send_target`:
`ALL_CLIENT=0`, `AREA=2`, `SELF=24`, …

### `timer` — Timers

| Function | Purpose |
|---|---|
| `gettick()` | Current tick (ms) |
| `add_timer(when_tick, func, id, data)` | One-shot |
| `add_timer_interval(when_tick, func, id, data, interval_ms)` | Repeating |
| `delete_timer(tid, func)` | Cancel |

Timer callbacks have signature
`int32_t (*)(int32_t tid, int64_t tick, int32_t id, intptr_t data)`.

### `log` — Logging

Wrappers around `ShowInfo`/`ShowStatus`/`ShowWarning`/`ShowError`/`ShowDebug`.
Format strings yourself first (e.g. with `snprintf`) and pass the
result — variadic format strings are not safe across DLL boundaries.

### `packet` — Custom packets

Install handlers on unused packet IDs and push outbound packets to clients.

| Function | Purpose |
|---|---|
| `register_handler(cmd, length, func, user_data)` | Map an inbound packet to your handler. `length=-1` for variable-length. `user_data` is forwarded to the handler. |
| `unregister_handler(cmd)` | Remove a handler |
| `read_b/w/l(fd, off)` | Read primitives from the recv buffer |
| `read_str(fd, off)` / `read_rest(fd)` | String pointer / bytes remaining |
| `send_self(fd, buf, len)` | Push a fully-formed packet to one fd |
| `send_target(bl, buf, len, target)` | Broadcast via `clif_send` |

The handler signature is
`void (*)(int32_t fd, struct map_session_data* sd)`. `sd` may be
`nullptr` for pre-login packets.

> **Choose unused IDs.** The valid range is `0x064`–`0xCFF`. Make sure
> the ID does not collide with anything your client build already uses.

---

## Patterns

### Closure data (`user_data`)

Every registration on this API accepts a `user_data` pointer that is
passed verbatim to the callback. It lets a single C function back
multiple registrations without resorting to globals — useful when a
plugin exposes the same logic with different configuration:

```cpp
struct shop_t { uint32_t bonus_item; int32_t multiplier; };
static shop_t small_shop{ 512, 1 }, big_shop{ 7227, 5 };

// Same C function, two registrations, two configs.
api->atcmd.register_cmd("smallreward", 0, on_reward, &small_shop);
api->atcmd.register_cmd("bigreward",   0, on_reward, &big_shop);
```

Every callback type here exposes the same closure mechanism:

| Where you register | Callback receives |
|---|---|
| `hook_add(..., user_data, ...)` | `cb(data, user_data)` |
| `script_addcommand(..., user_data)` | `func(st, user_data)` |
| `atcmd.register_cmd(..., user_data)` | `func(sd, cmd, msg, user_data)` |
| `packet.register_handler(..., user_data)` | `func(fd, sd, user_data)` |
| `map.foreachinmap(cb, user, ...)` | `cb(bl, user)` |
| `timer.add_timer(..., id, data)` | `func(tid, tick, id, data)` |

Pass `nullptr` if you do not need it.

### Custom script commands

```cpp
// plugin_hello "<name>";  →  prints to console, returns "hello!"
static int32_t buildin_plugin_hello(script_state* st, void* /*user_data*/) {
    const char* name = g_api->script.getstr(st, 2);
    char buf[128];
    snprintf(buf, sizeof(buf), "[plugin] hello, %s", name ? name : "world");
    g_api->log.info(buf);
    g_api->script.pushstr(st, "hello!");
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// In plugin_init:
api->script_addcommand("plugin_hello", "s", buildin_plugin_hello, /*user_data=*/nullptr);
```

The arg-string follows rAthena's existing convention:
`s` = string, `i` = integer, `?` = optional, `*` = variadic, etc.

### Custom @commands

```cpp
static int32_t on_atcmd_heal(map_session_data* sd, const char* cmd,
                             const char* msg, void* /*user_data*/) {
    g_api->status.heal(g_api->pc.as_bl(sd), 99999, 99999, 0);
    g_api->pc.message(g_api->pc.get_fd(sd), "Fully healed.");
    return 0;
}

// In plugin_init:
api->atcmd.register_cmd("plugin_heal", /*level=*/0, on_atcmd_heal, /*user_data=*/nullptr);
```

### Async script commands

A plugin script command can pause execution and resume later — useful
when waiting on a timer, network call, or external event.

```cpp
static int32_t on_tick(int32_t, int64_t, int32_t, intptr_t data) {
    void* token = reinterpret_cast<void*>(data);
    auto* st = static_cast<script_state*>(token);

    g_api->script.pushint(st, 42);     // push the return value first
    g_api->script.resume(token);       // then resume
    return 0;
}

static int32_t buildin_plugin_async(script_state* st, void* /*user_data*/) {
    void* token = g_api->script.suspend(st);
    g_api->timer.add_timer(g_api->timer.gettick() + 1000,
                           on_tick, 0, reinterpret_cast<intptr_t>(token));
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}
```

The player remains attached to the NPC while the script is parked
(matching `close` / `select` semantics). If the player logs out or
the NPC is reloaded, the engine frees the state automatically and any
later `resume()` becomes a no-op.

### Custom client packets

```cpp
static constexpr uint16_t MY_PACKET = 0x0CFE;     // pick something unused

static void on_my_packet(int32_t fd, map_session_data* sd, void* /*user_data*/) {
    if (!sd) return;
    uint32_t value = g_api->packet.read_l(fd, 2);  // skip 2-byte cmd

    // Echo back with value+1 on packet 0x0CFF
    struct __attribute__((packed)) { uint16_t cmd; uint32_t v; } reply;
    reply.cmd = 0x0CFF;
    reply.v   = value + 1;
    g_api->packet.send_self(g_api->pc.get_fd(sd), &reply, sizeof(reply));
}

// In plugin_init:
api->packet.register_handler(MY_PACKET, /*length=*/6, on_my_packet, /*user_data=*/nullptr);
```

---

## Example Plugin

The bundled [`plugins/example/example_plugin.cpp`](example/example_plugin.cpp)
demonstrates every category. Highlights:

| Script command | Demonstrates |
|---|---|
| `plugin_hello "x"` | Basic command + return value |
| `plugin_give_item id, n` | `pc.additem` |
| `plugin_spawn_mob id, n` | `mob.once_spawn` |
| `plugin_warp "map", x, y` | `pc.setpos` |
| `plugin_announce "msg"` | `clif.broadcast` |
| `plugin_count_mobs` | `map.foreachinmap` |
| `plugin_delayed_give id, secs` | `timer` + `clif.progressbar` |
| `plugin_async_roll(max)` | `script.suspend` / `script.resume` |
| `plugin_send_ping` | Outbound custom packet |

It also installs a handler for inbound packet `0x0CFE` to echo a reply
on `0x0CFF`.

Try it:

1. Uncomment the line in `conf/plugins.conf`:
   ```text
   plugins/example/example_plugin
   ```
2. `make plugin`
3. Restart map-server.

---

## Tips & Troubleshooting

- **Symbol not found / missing exports** — Make sure your three
  required exports use `PLUGIN_API` (which expands to
  `extern "C" __attribute__((visibility("default")))` on Linux and
  `__declspec(dllexport)` on Windows).
- **Plugin loaded, but commands don't work** — Confirm the plugin path
  in `conf/plugins.conf` is relative to the map-server working
  directory and that the file extension is omitted (the loader adds
  it). Check the startup log for `plugin: Loaded ...`.
- **Crashes on shutdown** — Most often a hook callback that wasn't
  removed in `plugin_final()`. Walk every `hook_add` and pair it with
  a matching `hook_remove`.
- **Variadic `printf` from a plugin shows garbage** — Use the `log`
  sub-struct and format strings yourself first; do not pass format
  specifiers across the DLL boundary.
- **Packet ID conflict** — `register_handler` overwrites whatever was
  in `packet_db[cmd]`. Pick a value that is not used by your client.
- **Hot reload** — `plugin_manager_init` and `plugin_manager_final` are
  the only entry points; current builds reload only on full server
  restart.

---

For the full ABI definition, hook payload structs and constants, see
[`src/map/plugin.hpp`](../src/map/plugin.hpp).
