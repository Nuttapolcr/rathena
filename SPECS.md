# SPECS.md — Feature log

Chronological log of what shipped in the Lua mod / workshop system: what was
built, when, which commit, and links to the knowledge notes it produced.

**How to use:** when you finish a new system or a notable feature, add a
section at the bottom. Newest entries last. Each entry:

```markdown
## <feature> — YYYY-MM-DD (commit <short hash>)

<One-paragraph description of what shipped and why.>

- Knowledge: [<note>](docs/knowledge/<slug>.md) — _(or "none")_
```

See [`CLAUDE.md`](CLAUDE.md) rule 3. The reusable, topical knowledge lives in
[`docs/knowledge/`](docs/knowledge/); this file is the time-ordered index of
features, not a knowledge store.

---

## workshop: Lua mod loader foundation — 2026-05-10 (commits 496d91b78, ff94d7a72, 42a5c370e)

Initial Lua mod loader for the rAthena map-server. Lua wrappers covering the
rAthena script API, NPC dialog, stat bonuses, variable storage, and
closure-based dispatch for scripts and atcommands.

- Knowledge: none recorded yet

## workshop: README + example mod scaffold — 2026-05-10 (commits 230b91338, 546b870f2)

README covering build, `modinfo.yml` schema, and the Lua API. Example mod
demonstrating dialog, bonus, and var-storage. See
[`plugins/workshop/README.md`](plugins/workshop/README.md).

- Knowledge: none recorded yet

## workshop: per-mod DB store + scripts — 2026-05-10 (commits 118d4d228, 5a1c1ebbe)

Per-mod YAML DB store with override modes; per-mod rAthena scripts and
`disable_scripts` via `modinfo.yml`.

- Knowledge: none recorded yet

## workshop: clock & timer events — 2026-05-10 (commit 4bfb6d0e0)

`OnClock` and `OnTimer` event support; `enabled` flag in `modinfo.yml`
(commit 004259486).

- Knowledge: none recorded yet

## workshop: Windows build support — 2026-05-11 (commits 16389ec06, baa8eebe9, 811774a7f, eb1501f6c)

Visual Studio project, portable filesystem ops, auto-fetch Lua on MSBuild,
`localtime_r` compat, header-dependency tracking so `plugin.hpp` edits rebuild.

- Knowledge: none recorded yet

## workshop: Lua status changes — 2026-05-11 (commits ee053ee38, ec90ce4bb)

Lua-defined status changes, name-keyed DB files (`status.yml`), and
`register_sc` for plugin-defined status changes.

- Knowledge: none recorded yet

## workshop: client packet hooks — 2026-05-11 (commit fd3af04dd)

Lua hooks for client packets — `on_packet` / `register_packet`.

- Knowledge: none recorded yet

## workshop: Lua battle config access — 2026-05-11 (commit 8f2b7ee9a)

Lua get/set/has for `conf/battle/*.conf` battle config values.

- Knowledge: none recorded yet

## workshop: storage/UI wrappers + intif hook — 2026-05-14 (commit edc1245b6)

Lua wrappers for the storage/UI plugin API and an `intif_connected` hook.

- Knowledge: none recorded yet

## workshop: Lua PacketWriter API — 2026-05-14 (commit 8b78a0c6b)

Lua `PacketWriter` API for custom packet construction. Example in
`plugins/workshop/examples/packet_writer_example.lua`.

- Knowledge: none recorded yet
