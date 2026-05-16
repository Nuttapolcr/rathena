# Workshop hook() — full event + ctx field map

**Area:** workshop / lua-api
**Last verified:** 2026-05-17 (commit plugin/workshop WIP)

`hook(name, fn)` binds a Lua handler to an engine `HOOK_*` event. The
Lua name → `e_plugin_hook` map lives in `hook_event_id_from_name`
(plugins/workshop/lua_globals.cpp) and the per-event ctx table is built
in `hook_dispatch` (same file, the big `switch (hook_type)`). A handler
returning `false` or `"stop"` yields `HOOK_STOP` — only events whose
engine call site checks `== HOOK_STOP` can actually be cancelled
(others are informational regardless of return).

ctx struct definitions: `src/map/plugin.hpp` (the `plugin_*_t`
structs). `push_player` pushes `nil` for a null `map_session_data*`,
so optional player/target fields are safe.

| Lua name | HOOK_* | ctx fields | engine fire site | cancellable |
|---|---|---|---|---|
| `pc_login` | HOOK_PC_LOGIN | player | src/map/pc.cpp:2308 | no |
| `pc_logout` | HOOK_PC_LOGOUT | player | src/map/map.cpp:2244 | no |
| `pc_baselevelup` | HOOK_PC_BASELEVELUP | player | src/map/pc.cpp:8328 | no |
| `pc_joblevelup` | HOOK_PC_JOBLEVELUP | player | src/map/pc.cpp:8386 | no |
| `pc_dead` | HOOK_PC_DEAD | player | src/map/pc.cpp:9828 | yes |
| `pc_chat` | HOOK_PC_CHAT | player, message | src/map/clif.cpp:11521 | yes |
| `pc_whisper` | HOOK_PC_WHISPER | player, target, message | src/map/clif.cpp:11878 | yes |
| `pc_partychat` | HOOK_PC_PARTYCHAT | player, message | src/map/clif.cpp:13990 | yes |
| `pc_guildchat` | HOOK_PC_GUILDCHAT | player, message | src/map/clif.cpp:14612 | yes |
| `mob_kill` | HOOK_MOB_KILL | mob_id, mob_name, killer | src/map/mob.cpp:2966 | yes |
| `mob_spawn` | HOOK_MOB_SPAWN | mob_id, mob_name | src/map/mob.cpp:1227 | no |
| `item_use` | HOOK_ITEM_USE | player, index | src/map/pc.cpp:6505 | yes |
| `item_pickup` | HOOK_ITEM_PICKUP | player, item_id, item_name, amount | src/map/pc.cpp:6011 | yes |
| `item_drop` | HOOK_ITEM_DROP | player, index, amount | src/map/pc.cpp:6177 | yes |
| `item_equip` | HOOK_ITEM_EQUIP | player, index, position | src/map/pc.cpp:12084 | yes |
| `skill_use` | HOOK_SKILL_USE | player (if caster is PC), skill_id, skill_lv | src/map/skill.cpp:4212, 4403 | yes |
| `npc_click` | HOOK_NPC_CLICK | player | src/map/npc.cpp:2244 | yes |
| `atcmd_execute` | HOOK_ATCMD_EXECUTE | player, command, params | src/map/atcommand.cpp:12062, 12112 | no |
| `trade_request` | HOOK_TRADE_REQUEST | player, target | src/map/trade.cpp:89 | yes |
| `trade_commit` | HOOK_TRADE_COMMIT | player, target | src/map/trade.cpp:637 | yes |
| `party_create` | HOOK_PARTY_CREATE | player, name | src/map/party.cpp:166 | yes |
| `party_leave` | HOOK_PARTY_LEAVE | player, party_id | src/map/party.cpp:808 | yes |
| `guild_create` | HOOK_GUILD_CREATE | player, name | src/map/guild.cpp:720 | yes |
| `guild_join` | HOOK_GUILD_JOIN | player, guild_id | src/map/guild.cpp:1162 | no |
| `guild_leave` | HOOK_GUILD_LEAVE | player, guild_id | src/map/guild.cpp:1200 | yes |
| `status_change_start` | HOOK_STATUS_CHANGE_START | player (if bl is PC), type, val1..val4, duration_ms | src/map/status.cpp:10203 | yes |
| `status_change_end` | HOOK_STATUS_CHANGE_END | player (if bl is PC), type, val1..val4, duration_ms (0) | src/map/status.cpp:13472 | no |
| `vending_open` | HOOK_VENDING_OPEN | player, message | src/map/vending.cpp:337 | yes |
| `vending_buy` | HOOK_VENDING_BUY | player, vendor | src/map/vending.cpp:150 | yes |
| `storage_open` | HOOK_STORAGE_OPEN | player | src/map/storage.cpp:148 | yes |
| `quest_add` | HOOK_QUEST_ADD | player, quest_id | src/map/quest.cpp:607 | yes |
| `quest_complete` | HOOK_QUEST_COMPLETE | player, quest_id | src/map/quest.cpp:870 | no |
| `pet_born` | HOOK_PET_BORN | player | src/map/pet.cpp:1097 | no |
| `pet_catch` | HOOK_PET_CATCH | player, item_id | src/map/pet.cpp:1228 | yes |
| `homun_call` | HOOK_HOMUN_CALL | player | src/map/homunculus.cpp:1141 | yes |
| `homun_levelup` | HOOK_HOMUN_LEVELUP | new_level | src/map/homunculus.cpp:521 | no |
| `intif_connected` / `char_reconnect` | HOOK_INTIF_CONNECTED | first (bool) | src/map/chrif.cpp (chrif_connectack/on_ready) | no |

## Gotchas

- `skill_use` and `status_change_start/end` carry a `block_list*`, not
  always a player. `hook_dispatch` resolves `player` only when
  `g_api->bl.get_type(bl) == PLUGIN_BL_PC`; for mob/homun casters
  `ctx.player` is absent — read `skill_id`/`type` instead.
- `homun_levelup` has no homunculus accessor in `plugin_api_t`, so ctx
  carries only `new_level`. Add a homun accessor to plugin.hpp if more
  is needed.
- HOOK_INTIF_CONNECTED / plugin_intif_connected_t were the engine-side
  half of the storage/UI/intif feature; they ship in plugin.hpp via
  the cherry-picked engine commit (the Lua side alone left the build
  broken — see workshop-missing-engine-api note).
- Engine fire-site line numbers drift across rAthena merges. The HOOK_*
  enum names are stable; re-grep `plugin_hook_fire(` if a line looks
  wrong.
