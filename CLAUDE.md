# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository

rAthena is a C++17 Ragnarok Online MMORPG server emulator, continuation of the eAthena project. It builds four cooperating server binaries (login, char, map, web) plus tooling, and ships a sizeable `db/` (YAML game database), `conf/` (runtime config), `npc/` (script content), and `sql-files/` (schema). A dynamic **plugin system** (`plugins/`) is a first-party extension surface on the map-server, layered on top of the vanilla source.

## Build & Run

The build is GNU autotools + Makefile on Linux/macOS (preferred for this repo's day-to-day) and CMake + Visual Studio on Windows; both are kept working in CI.

```bash
# One-time configure (generates Makefile from Makefile.in, detects MySQL/PCRE, etc.)
./configure                # add --enable-debug, --enable-vip, --with-mysql=... as needed

# Common targets — run from repo root
make server                # build login + char + map + web (+ import scaffolding)
make map                   # build only the map-server (most iteration happens here)
make char ; make login ; make web
make tools                 # src/tool/ + map-server tools (mapcache, etc.)
make plugin                # build every plugins/<name>/*.cpp into plugins/<name>/<name>.so
make import                # populate conf/import, conf/msg_conf/import, db/import from -tmpl
make clean
make help                  # full list of targets
```

CMake (cross-platform / Windows):

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .            # produces login-server, char-server, map-server, web-server binaries
```

Run servers from the repo root (binaries are written there; they expect relative `conf/`, `db/`, `npc/`):

```bash
./athena-start start       # launches login + char + map (uses fifos, writes pids/.log files)
./athena-start stop
./athena-start restart
# Or run a single binary directly: ./map-server, ./char-server, ./login-server, ./web-server
```

**Tests.** There is no unit-test framework in-tree. CI in `.github/workflows/` exercises the build matrix (gcc, clang, cmake, msbuild, packet versions, "modes", VIP). Validation is done via:

- `make` (all compilers, all targets) — the primary correctness gate.
- `npc_db_validation.yml` — boots the server and validates the YAML/NPC database loads.
- Manual in-game verification once the server boots.

If you change game logic, the realistic feedback loop is: `make map` → restart `./map-server` → reproduce in a client. There is no faster "single test" path.

## Architecture

### The four servers

rAthena splits the original Aegis monolith across cooperating processes that talk over TCP:

- **`src/login/`** → authenticates accounts, maintains the account DB, hands clients off to char-server.
- **`src/char/`** → manages characters, party/guild membership, inter-server routing of persistent state.
- **`src/map/`** → the gameplay server. By far the largest subsystem (~100 `.cpp` files). Owns mobs, skills, items, scripts, NPCs, packets to the client, battle math, status changes, storage, vending, instancing, etc.
- **`src/web/`** → HTTP endpoints (ranking, attendance, RODEX-style features) backed by the same MySQL.
- **`src/common/`** → shared utilities (sockets, timers, db abstraction, YAML loading, mapindex). Linked into every server.
- **`src/tool/`** + **`src/map/`** tool targets → mapcache builder, CSV ↔ YAML converters, etc.

The four servers can run on one host (default) or be split. `conf/inter_athena.conf` / `conf/<x>_athena.conf` and `db/inter_server.yml` configure how they find each other and the MySQL DB.

### Map-server internal shape

The map-server is event-loop driven (`src/common/socket.*` + `src/common/timer.*`) and is organized by gameplay subsystem rather than by layer — e.g. [src/map/pc.cpp](src/map/pc.cpp) owns players, [src/map/mob.cpp](src/map/mob.cpp) owns monsters, [src/map/skill.cpp](src/map/skill.cpp) owns skill execution. Two pieces unify everything:

- **`block_list` / `bl`** — the polymorphic base "thing on a map" (player, mob, NPC, item drop, skill unit, pet, …). Most cross-cutting code (movement, AoE, line-of-sight, damage routing, area iteration) operates on `block_list*` and switches on `bl->type`.
- **The script engine** in [src/map/script.cpp](src/map/script.cpp) — interprets the custom rAthena scripting language used by every NPC in `npc/`. It is reachable both from in-world events (NPC click, OnTimer, OnPCLogin, etc.) and from `@commands` ([src/map/atcommand.cpp](src/map/atcommand.cpp)).

The client packet boundary lives in [src/map/clif.cpp](src/map/clif.cpp) (~25k LOC). Packet versions are gated by `PACKETVER` macros — many functions have multiple code paths conditional on the configured client version (see `build_servers_packetversions.yml`).

### Configuration & data flow

- **`conf/`** — runtime tuneables consumed at boot (`battle/`, `inter_athena.conf`, `char_athena.conf`, `map_athena.conf`, `packet_athena.conf`, `plugins.conf`, …). Edit `conf/import/` (created by `make import`) to override without touching upstream files.
- **`db/`** — YAML game database (item_db, mob_db, skill_db, status, achievements, instance, …). Loaded once at boot, with `db/import/` as the local override path. Pre-re/re modes select between `db/pre-re/` and `db/re/` subtrees.
- **`npc/`** — server-side script content. `npc/scripts_main.conf` is the master include list; NPCs are compiled into the script engine at startup.
- **`sql-files/`** — schema and seed data for the MySQL database (logs, accounts, character storage, etc.).

When changing game balance or content, the answer is almost always "edit YAML in `db/` or a script under `npc/`," not "edit C++." Look for an existing pattern before adding a hardcoded value to `src/`.

### The plugin system

The plugin system is the project's escape hatch for adding map-server features without forking `src/`. The full developer-facing docs are in [plugins/README.md](plugins/README.md); the load order and integration points are:

- `plugin_manager_init()` is called from [src/map/map.cpp](src/map/map.cpp) **after** the script engine is up and **before** NPC compilation, so plugins can register `script_addcommand`s that NPCs then reference at compile time.
- `plugin_manager_final()` is called **before** any subsystem teardown, so plugins can clean up while every API they used is still live.
- Plugins never include map-server headers other than [src/map/plugin.hpp](src/map/plugin.hpp). All server types (`map_session_data`, `mob_data`, `block_list`, `script_state`, …) are forward-declared and reached through function pointers on a `plugin_api_t` struct passed into `plugin_init`. This keeps the plugin ABI independent of internal field layouts.
- Hooks are dispatched from inside the gameplay code via `plugin_hook_fire(HOOK_*, &payload)`. Returning `HOOK_STOP` from a hook vetoes the event for the hooks that fire **before** an action (item use, trade request, NPC click, mob kill, …); purely informational hooks (level-up, login) ignore the return.
- The plugin loader reads `conf/plugins.conf` (one path per line, extension auto-appended) and `dlopen` / `LoadLibrary`s each entry.

When adding a new hook point in the server: define the enum in [src/map/plugin.hpp](src/map/plugin.hpp), add a payload struct alongside it, call `plugin_hook_fire(HOOK_X, &payload)` at the call site, and respect a `HOOK_STOP` return if the event is vetoable. The receiving end (any plugin) only needs to recompile against the new header.

### Pre-re vs. re

The codebase supports two game eras ("pre-renewal" and "renewal"). The split is driven by:

- The `PRE` build flag (set by `./configure --enable-prere`, or the `PRERE` define in CMake / msbuild).
- Mirrored YAML trees under `db/pre-re/` and `db/re/`.
- Conditional code under `#ifdef RENEWAL` / `#ifndef RENEWAL` throughout the map-server, especially in [src/map/battle.cpp](src/map/battle.cpp), [src/map/status.cpp](src/map/status.cpp), and [src/map/skill.cpp](src/map/skill.cpp).

When touching battle, status, or skill formulas, check whether the change should apply to one mode or both, and grep both `#ifdef RENEWAL` arms.

## Conventions

- **Header style.** `.hpp` for headers, `.cpp` for sources. `using namespace std;` is avoided in headers but common in `.cpp`. C++17 is the floor (`CMAKE_CXX_STANDARD 17` in [CMakeLists.txt](CMakeLists.txt)).
- **No exceptions in hot paths.** The gameplay loop returns error codes; allocation failures use `aMalloc`/`aFree` wrappers in `src/common/malloc.*`.
- **Logging.** Use `ShowInfo` / `ShowStatus` / `ShowWarning` / `ShowError` / `ShowDebug` from `src/common/showmsg.*` — never `printf` directly. Plugins go through `api->log.*`.
- **Imports/overrides.** Don't edit a tracked `conf/<x>.conf` or `db/<x>.yml` if your change is install-local — put it in the matching `import/` folder so `git pull` stays clean. `make import` creates these folders on first run.
- **Feature documentation.** When adding a new feature, document it in [SPECS.md](SPECS.md) with:
  - **Feature name** — what was added
  - **Date** — when it was added (YYYY-MM-DD)
  - **Description** — what the feature does and why
  - **Files changed** — which files were modified
  - **Related commits** — git hashes or branch refs
  - **Research/knowledge** — key findings, architectural decisions, and how-to knowledge that informed the implementation (e.g., "packet structure uses little-endian encoding, stored in `struct PACKET_ZC_*`"; "plugin API accessed via `g_api->` function pointers"; "Lua userdata requires `luaL_newmetatable` + `__gc` for cleanup"). This knowledge base becomes reference material for future related work on this codebase.
- **PR scope.** [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) asks contributors to branch off a feature branch (never master), describe the problem and the fix, and link any related issue. Beware GitHub's `@mention` behaviour when discussing rAthena's `@commands` in PRs / issues — always quote them.
