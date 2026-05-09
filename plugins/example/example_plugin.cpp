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
 *   plugin_announce "Hello!";    // server-wide yellow broadcast
 *   plugin_count_mobs;           // count mobs on the attached player's map
 *   plugin_delayed_give 512, 5;  // give an Apple after 5 seconds
 *   .@n = plugin_async_roll(3);  // suspends script ~1s then resumes with a roll [1..N]
 *   plugin_send_ping;            // pushes a custom 0x0CFE packet to the player
 */

#include <cstdio>
#include <cstdlib>

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
	if (d->sd && d->it) {
		uint32_t nameid = g_api->item_api.get_nameid(d->it);
		const char* iname = g_api->item_api.db_get_ename(nameid);
		char buf[160];
		snprintf(buf, sizeof(buf), "item_pickup: %s picks %s (%u) x%d",
		         g_api->pc.get_name(d->sd), iname, nameid, d->amount);
		g_api->log.info(buf);
	}
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

// plugin_announce "<message>";
// Server-wide yellow broadcast — demonstrates clif.broadcast.
static int32_t buildin_plugin_announce(script_state* st)
{
	const char* msg = g_api->script.getstr(st, 2);
	if (msg) {
		// type=0 (BC_DEFAULT yellow), target=0 (ALL_CLIENT)
		g_api->clif.broadcast(nullptr, msg, 0, 0);
	}
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// plugin_count_mobs;
// Counts mobs on the attached player's map — demonstrates map.foreachinmap.
static int32_t buildin_plugin_count_mobs(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	auto count_cb = [](block_list* /*bl*/, void* user) -> int32_t {
		++*static_cast<int32_t*>(user);
		return 1;
	};

	int32_t count = 0;
	int16_t m = g_api->pc.get_mapid(sd);
	const int32_t BL_MOB_MASK = 1 << PLUGIN_BL_MOB; // type bitmask
	g_api->map.foreachinmap(count_cb, &count, m, BL_MOB_MASK);

	g_api->script.pushint(st, count);
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// Timer callback for plugin_delayed_give — fires once after the requested delay.
// `id` carries the player's account_id, `data` packs nameid (high) | amount (low).
static int32_t plugin_delayed_give_tick(int32_t /*tid*/, int64_t /*tick*/,
                                        int32_t id, intptr_t data)
{
	map_session_data* sd = g_api->map.id2sd(id);
	if (!sd) return 0;

	plugin_item_t it = {};
	it.nameid   = static_cast<uint32_t>((data >> 32) & 0xFFFFFFFF);
	it.identify = 1;
	int32_t amount = static_cast<int32_t>(data & 0xFFFFFFFF);

	g_api->pc.additem(sd, &it, amount, 7); // LOG_TYPE_SCRIPT
	g_api->clif.progressbar_abort(sd);
	g_api->pc.message(g_api->pc.get_fd(sd), "Your delayed item has arrived!");
	return 0;
}

// Pending async-roll request: holds the suspend token and the upper bound.
struct async_roll_t {
	void*   token;
	int32_t max;
};

// Timer callback that simulates an async result arriving after 1s.
// Pushes the random roll onto the script stack, then resumes execution.
static int32_t plugin_async_roll_tick(int32_t /*tid*/, int64_t /*tick*/,
                                      int32_t /*id*/, intptr_t data)
{
	auto* req = reinterpret_cast<async_roll_t*>(data);
	if (!req) return 0;

	// Pretend we got a result from an external service.
	int32_t result = 1 + (rand() % req->max);

	// Push the value first — script.resume() re-enters run_script_main,
	// which will then read this value as the command's return.
	// The token is owned by the suspended script_state; resume() needs
	// it to find the correct st pointer.
	void* token = req->token;
	delete req;

	// Recover the script_state from the token to push onto its stack.
	// suspend() returns the script_state* itself, so we can cast back.
	g_api->script.pushint(static_cast<script_state*>(token), result);
	g_api->script.resume(token);
	return 0;
}

// .@n = plugin_async_roll(<max>);
// Demonstrates script.suspend / script.resume: the script pauses at this
// command and resumes ~1 second later with a random integer in [1..max].
static int32_t buildin_plugin_async_roll(script_state* st)
{
	auto max = static_cast<int32_t>(g_api->script.getnum(st, 2));
	if (max < 1) max = 1;

	void* token = g_api->script.suspend(st);
	if (!token) {
		// Should never happen, but stay safe.
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	auto* req = new async_roll_t{ token, max };
	int64_t when = g_api->timer.gettick() + 1000; // 1s
	g_api->timer.add_timer(when, plugin_async_roll_tick, 0,
	                       reinterpret_cast<intptr_t>(req));

	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// ---- Custom client packet demo ----
//
// Picks an unused id near the top of the packet table. A real deployment
// should coordinate this with the client's packet table; here we just pick
// 0x0CFE for inbound (client -> server) and 0x0CFF for outbound (server ->
// client).
static constexpr uint16_t PLUGIN_PACKET_HELLO = 0x0CFE; // fixed-length, 6 bytes
static constexpr uint16_t PLUGIN_PACKET_PING  = 0x0CFF; // fixed-length, 6 bytes

#pragma pack(push, 1)
struct plugin_pkt_ping_t {
	uint16_t cmd;     // 0x0CFF
	uint32_t value;
};
#pragma pack(pop)

// Handler for inbound 0x0CFE: <cmd:2><value:4> = 6 bytes total.
// Logs what arrived and echoes back a 0x0CFF with value+1.
static void on_plugin_packet_hello(int32_t fd, map_session_data* sd)
{
	uint32_t value = g_api->packet.read_l(fd, 2);

	char buf[128];
	snprintf(buf, sizeof(buf), "[example] packet 0x%04x from %s: value=%u",
	         PLUGIN_PACKET_HELLO,
	         sd ? g_api->pc.get_name(sd) : "(no session)", value);
	g_api->log.info(buf);

	if (!sd) return;

	plugin_pkt_ping_t reply{};
	reply.cmd   = PLUGIN_PACKET_PING;
	reply.value = value + 1;
	g_api->packet.send_self(g_api->pc.get_fd(sd), &reply, sizeof(reply));
}

// plugin_send_ping;
// Pushes a 0x0CFF packet to the attached player. Demonstrates the
// outbound side of the API; the client only needs to know how to parse
// the 6-byte packet to react.
static int32_t buildin_plugin_send_ping(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	plugin_pkt_ping_t pkt{};
	pkt.cmd   = PLUGIN_PACKET_PING;
	pkt.value = 42;
	g_api->packet.send_self(g_api->pc.get_fd(sd), &pkt, sizeof(pkt));

	g_api->script.pushint(st, 1);
	return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// plugin_delayed_give <item_id>, <delay_seconds>;
// Demonstrates timer.add_timer + clif.progressbar.
static int32_t buildin_plugin_delayed_give(script_state* st)
{
	map_session_data* sd = g_api->script.rid2sd(st);
	if (!sd) {
		g_api->script.pushint(st, 0);
		return PLUGIN_SCRIPT_CMD_SUCCESS;
	}

	auto nameid = static_cast<uint32_t>(g_api->script.getnum(st, 2));
	auto secs   = static_cast<int32_t>(g_api->script.getnum(st, 3));
	if (secs <= 0) secs = 1;

	// 0x00FF00 = green progressbar
	g_api->clif.progressbar(sd, 0x00FF00, secs);

	intptr_t data = (static_cast<intptr_t>(nameid) << 32) | 1; // amount=1
	int64_t  when = g_api->timer.gettick() + (int64_t)secs * 1000;
	g_api->timer.add_timer(when, plugin_delayed_give_tick,
	                       g_api->pc.get_aid(sd), data);

	g_api->script.pushint(st, 1);
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

	api->script_addcommand("plugin_hello",         "s",   buildin_plugin_hello);
	api->script_addcommand("plugin_give_item",     "ii",  buildin_plugin_give_item);
	api->script_addcommand("plugin_spawn_mob",     "ii",  buildin_plugin_spawn_mob);
	api->script_addcommand("plugin_warp",          "sii", buildin_plugin_warp);
	api->script_addcommand("plugin_announce",      "s",   buildin_plugin_announce);
	api->script_addcommand("plugin_count_mobs",    "",    buildin_plugin_count_mobs);
	api->script_addcommand("plugin_delayed_give",  "ii",  buildin_plugin_delayed_give);
	api->script_addcommand("plugin_async_roll",    "i",   buildin_plugin_async_roll);
	api->script_addcommand("plugin_send_ping",     "",    buildin_plugin_send_ping);

	// Install a fixed-length 6-byte handler for our custom inbound packet.
	// Cleared automatically by plugin_manager_final on shutdown / reload.
	api->packet.register_handler(PLUGIN_PACKET_HELLO, 6, on_plugin_packet_hello);

	api->log.status("[example] Plugin loaded. New commands: plugin_announce, plugin_count_mobs, plugin_delayed_give, plugin_async_roll, plugin_send_ping");
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
