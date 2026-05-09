/*
 * rathena Example Plugin
 * Demonstrates hooks, custom script commands, and the server API.
 * This file only depends on <map/plugin.hpp> — no server headers required.
 *
 * Enable by adding to conf/plugins.conf:
 *   plugins/example/example_plugin
 *
 * NPC usage example:
 *   plugin_hello "World";        // prints greeting, returns "hello!"
 *   plugin_give_item 512, 10;    // give 10 Apples to the attached player
 *   plugin_spawn_mob 1002, 3;    // spawn 3 Porings at the player's position
 *   plugin_warp "prontera", 156, 191;
 */

#include <cstdio>

#include <map/plugin.hpp>

// ---- Plugin metadata ----

static plugin_info_t info = {
	"Example Plugin",
	"rAthena Dev Team",
	"3.0.0",
	"Demonstrates every hook type, custom script commands, and the server API."
};

PLUGIN_API plugin_info_t* plugin_info() { return &info; }

// ---- Shared API pointer (set during plugin_init) ----

static plugin_api_t* g_api = nullptr;

// ---- Hook callbacks ----

static int on_mob_kill(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_mob_kill_t*>(data);
	if (!d->md) return HOOK_CONTINUE;

	printf("[example] mob_kill: %s (mob_id=%d)\n",
	       g_api->mob.get_name(d->md), g_api->mob.get_id(d->md));

	// If the killer is a player, give bonus EXP
	if (d->src && g_api->bl.get_type(d->src) == PLUGIN_BL_PC) {
		auto* sd = g_api->bl.as_sd(d->src);
		if (sd) {
			g_api->pc.gainexp(sd, nullptr, 100, 0, 0);
			g_api->pc.message(g_api->pc.get_fd(sd), "Bonus EXP from plugin!");
		}
	}

	return HOOK_CONTINUE;
}

static int on_mob_spawn(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_mob_spawn_t*>(data);
	if (d->md)
		printf("[example] mob_spawn: %s at (%d,%d)\n",
		       g_api->mob.get_name(d->md),
		       (int)g_api->mob.get_x(d->md),
		       (int)g_api->mob.get_y(d->md));
	return HOOK_CONTINUE;
}

static int on_pc_login(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_pc_login_t*>(data);
	if (!d->sd) return HOOK_CONTINUE;

	printf("[example] pc_login: %s (AID=%d)\n",
	       g_api->pc.get_name(d->sd), g_api->pc.get_aid(d->sd));

	g_api->status.heal(g_api->pc.as_bl(d->sd), 10000, 5000, 0);
	g_api->pc.message(g_api->pc.get_fd(d->sd), "Welcome back! You have been fully healed.");
	return HOOK_CONTINUE;
}

static int on_pc_logout(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_pc_logout_t*>(data);
	if (d->sd)
		printf("[example] pc_logout: %s\n", g_api->pc.get_name(d->sd));
	return HOOK_CONTINUE;
}

static int on_pc_baselevelup(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_pc_levelup_t*>(data);
	if (!d->sd) return HOOK_CONTINUE;
	printf("[example] base_levelup: %s -> lv %u\n",
	       g_api->pc.get_name(d->sd), g_api->pc.get_blv(d->sd));
	return HOOK_CONTINUE;
}

static int on_pc_joblevelup(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_pc_levelup_t*>(data);
	if (!d->sd) return HOOK_CONTINUE;
	printf("[example] job_levelup: %s -> jlv %u\n",
	       g_api->pc.get_name(d->sd), g_api->pc.get_jlv(d->sd));
	return HOOK_CONTINUE;
}

static int on_item_use(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_item_use_t*>(data);
	if (d->sd)
		printf("[example] item_use: %s uses slot %d\n",
		       g_api->pc.get_name(d->sd), d->index);
	return HOOK_CONTINUE;
}

static int on_item_pickup(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_item_pickup_t*>(data);
	if (d->sd && d->it)
		printf("[example] item_pickup: %s picks %u x%d\n",
		       g_api->pc.get_name(d->sd), g_api->item_api.get_nameid(d->it), d->amount);
	return HOOK_CONTINUE;
}

static int on_item_drop(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_item_drop_t*>(data);
	if (d->sd)
		printf("[example] item_drop: %s drops slot %d x%d\n",
		       g_api->pc.get_name(d->sd), d->index, d->amount);
	return HOOK_CONTINUE;
}

static int on_item_equip(void* data, void* /*user_data*/)
{
	auto* d = static_cast<plugin_item_equip_t*>(data);
	if (d->sd)
		printf("[example] item_equip: %s equips slot %d (pos 0x%x)\n",
		       g_api->pc.get_name(d->sd), (int)d->index, (unsigned)d->position);
	return HOOK_CONTINUE;
}

// ---- Custom NPC script commands ----

// plugin_hello "<name>";
// Returns the string "hello!".
static int32_t buildin_plugin_hello(script_state* st)
{
	const char* name = g_api->script.getstr(st, 2);
	printf("[example] Hello, %s!\n", name ? name : "world");
	g_api->script.pushstr(st, "hello!");
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// plugin_give_item <item_id>, <amount>;
// Gives the attached player <amount> of <item_id>. Returns 1 on success.
static int32_t buildin_plugin_give_item(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	plugin_item_t it = {};
	it.nameid   = static_cast<uint32_t>(g_api->script.getnum(st, 2));
	it.identify = 1;

	auto amount = static_cast<int32_t>(g_api->script.getnum(st, 3));
	int result = g_api->pc.additem(sd, &it, amount, 7); // LOG_TYPE_SCRIPT=7
	g_api->script.pushint(st, result == 0 ? 1 : 0);
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// plugin_spawn_mob <mob_id>, <amount>;
// Spawns monsters at the attached player's location.
static int32_t buildin_plugin_spawn_mob(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	auto mob_id = static_cast<int32_t>(g_api->script.getnum(st, 2));
	auto amount = static_cast<int32_t>(g_api->script.getnum(st, 3));

	int32_t spawned = g_api->mob.once_spawn(sd,
	    g_api->pc.get_mapid(sd),
	    g_api->pc.get_pos_x(sd),
	    g_api->pc.get_pos_y(sd),
	    "", mob_id, amount, "", 0, 0);

	g_api->script.pushint(st, spawned);
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// plugin_warp "<mapname>", <x>, <y>;
// Warps the attached player to the given position.
static int32_t buildin_plugin_warp(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	const char* mapname = g_api->script.getstr(st, 2);
	auto x = static_cast<int32_t>(g_api->script.getnum(st, 3));
	auto y = static_cast<int32_t>(g_api->script.getnum(st, 4));

	uint16_t mapidx = g_api->map.name2id(mapname);
	int result = g_api->pc.setpos(sd, mapidx, x, y, 0); // CLR_TELEPORT=0
	g_api->script.pushint(st, result == 0 ? 1 : 0);
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// ---- Plugin lifecycle ----

PLUGIN_API bool plugin_init(plugin_api_t* api)
{
	g_api = api;

	api->hook_add(HOOK_MOB_KILL,       on_mob_kill,       nullptr, 100);
	api->hook_add(HOOK_MOB_SPAWN,      on_mob_spawn,      nullptr, 100);
	api->hook_add(HOOK_PC_LOGIN,       on_pc_login,       nullptr, 100);
	api->hook_add(HOOK_PC_LOGOUT,      on_pc_logout,      nullptr, 100);
	api->hook_add(HOOK_PC_BASELEVELUP, on_pc_baselevelup, nullptr, 100);
	api->hook_add(HOOK_PC_JOBLEVELUP,  on_pc_joblevelup,  nullptr, 100);
	api->hook_add(HOOK_ITEM_USE,       on_item_use,       nullptr, 100);
	api->hook_add(HOOK_ITEM_PICKUP,    on_item_pickup,    nullptr, 100);
	api->hook_add(HOOK_ITEM_DROP,      on_item_drop,      nullptr, 100);
	api->hook_add(HOOK_ITEM_EQUIP,     on_item_equip,     nullptr, 100);

	api->script_addcommand("plugin_hello",      "s",   buildin_plugin_hello);
	api->script_addcommand("plugin_give_item",  "ii",  buildin_plugin_give_item);
	api->script_addcommand("plugin_spawn_mob",  "ii",  buildin_plugin_spawn_mob);
	api->script_addcommand("plugin_warp",       "sii", buildin_plugin_warp);

	printf("[example] Plugin loaded. Commands: plugin_hello, plugin_give_item, plugin_spawn_mob, plugin_warp\n");
	return true;
}

PLUGIN_API void plugin_final()
{
	if (g_api) {
		g_api->hook_remove(HOOK_MOB_KILL,       on_mob_kill);
		g_api->hook_remove(HOOK_MOB_SPAWN,      on_mob_spawn);
		g_api->hook_remove(HOOK_PC_LOGIN,       on_pc_login);
		g_api->hook_remove(HOOK_PC_LOGOUT,      on_pc_logout);
		g_api->hook_remove(HOOK_PC_BASELEVELUP, on_pc_baselevelup);
		g_api->hook_remove(HOOK_PC_JOBLEVELUP,  on_pc_joblevelup);
		g_api->hook_remove(HOOK_ITEM_USE,       on_item_use);
		g_api->hook_remove(HOOK_ITEM_PICKUP,    on_item_pickup);
		g_api->hook_remove(HOOK_ITEM_DROP,      on_item_drop);
		g_api->hook_remove(HOOK_ITEM_EQUIP,     on_item_equip);
	}
	printf("[example] Plugin unloaded.\n");
}
