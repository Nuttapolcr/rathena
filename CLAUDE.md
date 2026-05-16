# CLAUDE.md

This is rAthena (C++ MMORPG server) extended with a **Lua mod system** —
`plugins/workshop/`, a Lua mod loader for the map-server. Most active work
happens in mods and the workshop plugin, not the rAthena core.

---

## Workflow rules — mandatory for every AI agent

Follow these on **every** task. They keep knowledge from being re-derived and
keep new work documented.

### 1. Read before you work

Before writing any code, scan [`docs/knowledge/README.md`](docs/knowledge/README.md)
and its [`INDEX.md`](docs/knowledge/INDEX.md). Read every note whose topic
touches your task. The knowledge base holds packet layouts, plugin API shapes,
Lua binding patterns, lifecycle ordering, and gotchas that were expensive to
discover. Don't re-trace what's already written.

### 2. Record what you learn

When you discover something **non-obvious** — anything you had to read source,
experiment, or trace to find out (packet structure, API shape, lifecycle
ordering, a gotcha) — write it down before moving on:

- Create `docs/knowledge/<slug>.md`, one topic per file, using the file
  format defined in [`docs/knowledge/README.md`](docs/knowledge/README.md).
- Add a row to [`docs/knowledge/INDEX.md`](docs/knowledge/INDEX.md).
- If a note goes stale, fix it in place. Wrong knowledge is worse than none.

### 3. New system → write a README + log it

When you build a new subsystem under `plugins/workshop/` (or any new system):

- Ship a `README.md` for that subsystem describing what it does and how to
  use it.
- Add an entry to [`SPECS.md`](SPECS.md) — the chronological feature log —
  with the date, commit, a short description, and links to any knowledge
  notes the work produced.

---

## Doc map

| File | Purpose |
|------|---------|
| `CLAUDE.md` (this file) / `AGENTS.md` | Entry point + the 3 rules above. |
| [`docs/knowledge/README.md`](docs/knowledge/README.md) | How the knowledge base works + note format. **Single source of truth for rule 1 & 2.** |
| [`docs/knowledge/INDEX.md`](docs/knowledge/INDEX.md) | Index of all knowledge notes. |
| [`SPECS.md`](SPECS.md) | Chronological feature log (what shipped, when, which commit). |
| [`plugins/workshop/README.md`](plugins/workshop/README.md) | User-facing Lua API guide for mod authors. |

This file only points to those — it does not duplicate their content.

---

## Build & test

Build the workshop plugin (see [`plugins/workshop/README.md`](plugins/workshop/README.md)
§Build & install for the authoritative steps):

```sh
cd plugins/workshop && make
```

The build also exists as a Visual Studio project (`workshop.vcxproj`) for
Windows. Lua sources are auto-fetched (`fetch_lua.sh` / `fetch_lua.bat`).
