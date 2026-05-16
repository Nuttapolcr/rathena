# Knowledge Index

Every `.md` note in this folder, one row each. Scan this before starting work;
read any note whose topic touches your task. See
[`README.md`](README.md) for the note format and conventions.

| File | Topic | Area |
|------|-------|------|
| [workshop-hook-context-map.md](workshop-hook-context-map.md) | Every `hook()` event name → HOOK_* → ctx fields → engine fire site | workshop / lua-api |
| [workshop-missing-engine-api.md](workshop-missing-engine-api.md) | Storage/UI/intif Lua API shipped without its engine half; build-break + fix | plugin-loader / workshop |
| [workshop-packet-api.md](workshop-packet-api.md) | Client packet decode/encode/custom-id Lua API + reload lifetime | workshop / lua-api / packets |
| [workshop-script-eval.md](workshop-script-eval.md) | `script_eval`/`rathena()` — run any rAthena script snippet from Lua (call-anything bridge) | workshop / lua-api / script-bridge |
| [workshop-script-config-events.md](workshop-script-config-events.md) | `on_event`/`npc_event_all` — rAthena script_config + clock labels in Lua; clock dedup | workshop / lua-api / npc-events |
