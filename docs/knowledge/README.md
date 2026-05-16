# Knowledge Base

> **AI agents: read this folder before starting any work on this repo.**
> Every `.md` file here is durable technical knowledge discovered while building
> the Lua mod / workshop system. Treat it as reference material — check for an
> existing note before re-deriving an API, packet layout, or constraint.

## How to use this folder

1. **Before working:** scan [`INDEX.md`](INDEX.md) and read any note whose
   topic touches your task.
2. **After learning something non-obvious:** record it as a new `.md` file here
   (one topic per file) and add a line to `INDEX.md`. "Non-obvious" = anything
   you had to read source, experiment, or trace to find out — packet structure,
   plugin API shape, Lua binding patterns, lifecycle ordering, gotchas.
3. **When a note goes stale:** fix it in place. Wrong knowledge is worse than
   none. Notes describe what was true when written — verify identifiers still
   exist before relying on one.

## File format

```markdown
# <Topic title>

**Area:** <subsystem, e.g. workshop / lua-api / packets / plugin-loader>
**Last verified:** YYYY-MM-DD (commit <short hash>)

<The knowledge. Concrete. Include code paths as file:line, exact function
names, exact struct names. Show minimal examples where they clarify.>
```

Filename: short kebab-case slug describing the topic
(e.g. `lua-userdata-gc.md`, `packet-little-endian.md`).

## Relationship to other docs

- **`CLAUDE.md` / `AGENTS.md`** (repo root) — entry point and conventions.
  Points here.
- **`SPECS.md`** (repo root) — chronological *feature log* (what shipped, when,
  which commits). Each feature links to the knowledge notes it produced.
- **`docs/knowledge/`** (this folder) — *topical* knowledge, decoupled from
  any single feature. The reusable reference.
- **`plugins/workshop/README.md`** — user-facing Lua API guide for mod authors.
- **Per-subsystem `README.md`** — every C++ subsystem under `plugins/workshop/`
  ships its own README describing that subsystem.

## INDEX

The index lives in its own file: [`INDEX.md`](INDEX.md). Add a row there
whenever you create a note.
