// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef MAP_PLUGIN_HPP
#define MAP_PLUGIN_HPP

#include <cstdint>

// DLL export macro for plugin shared libraries
#ifdef _WIN32
#  define PLUGIN_API extern "C" __declspec(dllexport)
#else
#  define PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

// Hook dispatch results
#define HOOK_CONTINUE 0
#define HOOK_STOP     1

// Sentinel stored in str_data[n].val to mark plugin-registered script commands.
// Must be larger than any real buildin_func[] index (currently < 4000 entries).
static constexpr int64_t PLUGIN_CMD_BASE = 0x40000000LL;

// SCRIPT_CMD_SUCCESS / SCRIPT_CMD_FAILURE values for custom script commands
#ifndef PLUGIN_SCRIPT_CMD_SUCCESS
#  define PLUGIN_SCRIPT_CMD_SUCCESS 0
#  define PLUGIN_SCRIPT_CMD_FAILURE 1
#endif

// ---- Hook type identifiers ----

enum e_plugin_hook {
	// Mob
	HOOK_MOB_KILL = 0,      // monster is killed
	HOOK_MOB_SPAWN,         // monster finishes spawning on map

	// Player lifecycle
	HOOK_PC_LOGIN,          // player character logs in
	HOOK_PC_LOGOUT,         // player character logs out
	HOOK_PC_BASELEVELUP,    // player gains base level(s)
	HOOK_PC_JOBLEVELUP,     // player gains job level(s)
	HOOK_PC_DEAD,           // player dies

	// Inventory
	HOOK_ITEM_USE,          // player uses a consumable item
	HOOK_ITEM_PICKUP,       // player receives an item (any source)
	HOOK_ITEM_DROP,         // player drops an item
	HOOK_ITEM_EQUIP,        // player equips an item

	// Skills
	HOOK_SKILL_USE,         // skill execution begins (damage or non-damage)

	// Chat
	HOOK_PC_CHAT,           // player sends area/global chat
	HOOK_PC_WHISPER,        // player sends a whisper
	HOOK_PC_PARTYCHAT,      // player sends party chat
	HOOK_PC_GUILDCHAT,      // player sends guild chat

	// NPC
	HOOK_NPC_CLICK,         // player clicks an NPC

	// Trade
	HOOK_TRADE_REQUEST,     // player requests a trade
	HOOK_TRADE_COMMIT,      // trade is about to be executed

	// Party
	HOOK_PARTY_CREATE,      // party is being created
	HOOK_PARTY_LEAVE,       // player leaves a party

	// Guild
	HOOK_GUILD_CREATE,      // guild is being created
	HOOK_GUILD_JOIN,        // player joins a guild (confirmed by char-server)
	HOOK_GUILD_LEAVE,       // player leaves a guild

	// Status effects
	HOOK_STATUS_CHANGE_START, // status effect (buff/debuff) is about to be applied
	HOOK_STATUS_CHANGE_END,   // status effect is about to be removed/expired

	// Vending
	HOOK_VENDING_OPEN,      // player opens a vending shop
	HOOK_VENDING_BUY,       // player purchases from a vending shop

	// Storage
	HOOK_STORAGE_OPEN,      // player opens personal storage

	// Quests
	HOOK_QUEST_ADD,         // quest added to player's log
	HOOK_QUEST_COMPLETE,    // player completes a quest

	// Companions
	HOOK_PET_BORN,          // player hatches a pet egg
	HOOK_PET_CATCH,         // player begins catching a wild pet
	HOOK_HOMUN_CALL,        // player calls their homunculus
	HOOK_HOMUN_LEVELUP,     // homunculus gains a level

	// Commands
	HOOK_ATCMD_EXECUTE,     // @command or #command executed (informational)

	HOOK_MAX
};

// Forward declarations of server types (opaque to plugins)
struct mob_data;
class  map_session_data;   // declared as class in mmo.hpp
struct block_list;
struct item;
struct script_state;
struct npc_data;
struct homun_data;

// ============================================================
// Block list entity type constants (match e_bl_type in map.hpp)
// Use with api->bl.get_type() to identify what kind of entity a block_list* is.
// ============================================================
enum e_plugin_bl_type : int32_t {
	PLUGIN_BL_PC   = 4,
	PLUGIN_BL_MOB  = 5,
	PLUGIN_BL_PET  = 6,
	PLUGIN_BL_NPC  = 7,
	PLUGIN_BL_HOM  = 8,
	PLUGIN_BL_MER  = 9,
	PLUGIN_BL_ELEM = 10,
};

// Minimal item descriptor for pc.additem — no server headers required.
struct plugin_item_t {
	uint32_t nameid;    // item ID
	int8_t   identify;  // 1 = identified, 0 = unidentified
};

// ============================================================
// Hook event data structs
// ============================================================

// HOOK_MOB_KILL
struct plugin_mob_kill_t {
	struct mob_data*       md;    // monster that died
	struct block_list*     src;   // entity that dealt the killing blow (may be null)
	int32_t                type;  // death flags passed to mob_dead()
};

// HOOK_MOB_SPAWN
struct plugin_mob_spawn_t {
	struct mob_data*       md;    // monster that just appeared
};

// HOOK_PC_LOGIN
struct plugin_pc_login_t {
	struct map_session_data* sd;
};

// HOOK_PC_LOGOUT
struct plugin_pc_logout_t {
	struct map_session_data* sd;
};

// HOOK_PC_BASELEVELUP, HOOK_PC_JOBLEVELUP
struct plugin_pc_levelup_t {
	struct map_session_data* sd;
	int level_type;   // 0 = base level, 1 = job level
};

// HOOK_PC_DEAD — HOOK_STOP prevents the death from being processed
struct plugin_pc_dead_t {
	struct map_session_data* sd;
	struct block_list*       src;   // killer (may be null)
};

// HOOK_ITEM_USE — HOOK_STOP cancels item use
struct plugin_item_use_t {
	struct map_session_data* sd;
	int32_t index;    // inventory slot index
};

// HOOK_ITEM_PICKUP — HOOK_STOP cancels pickup
struct plugin_item_pickup_t {
	struct map_session_data* sd;
	struct item*  it;     // item being added
	int32_t       amount;
};

// HOOK_ITEM_DROP — HOOK_STOP cancels drop
struct plugin_item_drop_t {
	struct map_session_data* sd;
	int32_t index;    // inventory slot index
	int32_t amount;
};

// HOOK_ITEM_EQUIP — HOOK_STOP cancels equip
struct plugin_item_equip_t {
	struct map_session_data* sd;
	int16_t index;    // inventory slot index
	int32_t position; // equipment position bitmask
};

// HOOK_SKILL_USE — HOOK_STOP cancels skill execution
struct plugin_skill_use_t {
	struct block_list*       src;       // skill caster
	struct block_list*       bl;        // primary target
	uint16_t                 skill_id;
	uint16_t                 skill_lv;
};

// HOOK_PC_CHAT — HOOK_STOP cancels the message
struct plugin_pc_chat_t {
	struct map_session_data* sd;
	const char*              message;   // text only (without "name : " prefix)
};

// HOOK_PC_WHISPER — HOOK_STOP cancels the whisper
struct plugin_pc_whisper_t {
	struct map_session_data* sd;        // sender
	const char*              target;    // target character name
	const char*              message;
};

// HOOK_PC_PARTYCHAT — HOOK_STOP cancels the message
struct plugin_pc_partychat_t {
	struct map_session_data* sd;
	const char*              message;
};

// HOOK_PC_GUILDCHAT — HOOK_STOP cancels the message
struct plugin_pc_guildchat_t {
	struct map_session_data* sd;
	const char*              message;
};

// HOOK_NPC_CLICK — HOOK_STOP cancels the NPC interaction
struct plugin_npc_click_t {
	struct map_session_data* sd;
	struct npc_data*         nd;
};

// HOOK_TRADE_REQUEST — HOOK_STOP rejects the trade request
struct plugin_trade_request_t {
	struct map_session_data* sd;         // requester
	struct map_session_data* target_sd;  // receiver
};

// HOOK_TRADE_COMMIT — HOOK_STOP cancels the trade execution
struct plugin_trade_commit_t {
	struct map_session_data* sd;
	struct map_session_data* tsd;
};

// HOOK_PARTY_CREATE — HOOK_STOP rejects party creation
struct plugin_party_create_t {
	struct map_session_data* sd;
	const char*              name;
};

// HOOK_PARTY_LEAVE — HOOK_STOP prevents the player from leaving
struct plugin_party_leave_t {
	struct map_session_data* sd;
	int32_t                  party_id;
};

// HOOK_GUILD_CREATE — HOOK_STOP rejects guild creation
struct plugin_guild_create_t {
	struct map_session_data* sd;
	const char*              name;
};

// HOOK_GUILD_JOIN — informational (join already confirmed by char-server)
struct plugin_guild_join_t {
	struct map_session_data* sd;
	int32_t                  guild_id;
};

// HOOK_GUILD_LEAVE — HOOK_STOP prevents the player from leaving
struct plugin_guild_leave_t {
	struct map_session_data* sd;
	int32_t                  guild_id;
};

// HOOK_STATUS_CHANGE_START — HOOK_STOP rejects the status application
// type maps to sc_type enum values
struct plugin_status_change_t {
	struct block_list*       bl;          // entity receiving the status
	int32_t                  type;        // sc_type cast to int32
	int32_t                  val1;
	int32_t                  val2;
	int32_t                  val3;
	int32_t                  val4;
	int64_t                  duration_ms; // 0 when used for HOOK_STATUS_CHANGE_END
};

// HOOK_VENDING_OPEN — HOOK_STOP cancels shop opening (returns failure code)
struct plugin_vending_open_t {
	struct map_session_data* sd;
	const char*              message;    // shop title / description
};

// HOOK_VENDING_BUY — HOOK_STOP cancels the purchase
struct plugin_vending_buy_t {
	struct map_session_data* sd;         // buyer
	struct map_session_data* vsd;        // vendor
};

// HOOK_STORAGE_OPEN — HOOK_STOP denies storage access
struct plugin_storage_open_t {
	struct map_session_data* sd;
};

// HOOK_QUEST_ADD — HOOK_STOP rejects the quest addition
struct plugin_quest_add_t {
	struct map_session_data* sd;
	int32_t                  quest_id;
};

// HOOK_QUEST_COMPLETE — informational
struct plugin_quest_complete_t {
	struct map_session_data* sd;
	int32_t                  quest_id;
};

// HOOK_PET_BORN — informational
struct plugin_pet_born_t {
	struct map_session_data* sd;
};

// HOOK_PET_CATCH — HOOK_STOP cancels the catch attempt
struct plugin_pet_catch_t {
	struct map_session_data* sd;
	uint32_t                 item_id;    // taming item used
};

// HOOK_HOMUN_CALL — HOOK_STOP cancels the summon
struct plugin_homun_call_t {
	struct map_session_data* sd;
};

// HOOK_HOMUN_LEVELUP — informational
struct plugin_homun_levelup_t {
	struct homun_data*       hd;
	int32_t                  new_level;
};

// HOOK_ATCMD_EXECUTE — informational
struct plugin_atcmd_execute_t {
	struct map_session_data* sd;         // player executing the command
	const char*              command;    // full command token (e.g. "@warp")
	const char*              params;     // parameters after the command
};

// ============================================================
// Callback types
// ============================================================

typedef int (*plugin_hook_cb)(void* data, void* user_data);
typedef int32_t (*plugin_script_func)(struct script_state* st);
typedef int32_t (*plugin_atcmd_func)(struct map_session_data* sd,
                                     const char* command,
                                     const char* message);

// Block-list iteration callback (for map.foreachinmap / foreachinarea).
// Return value is summed by the caller; non-zero typically indicates "matched".
typedef int32_t (*plugin_blcb)(struct block_list* bl, void* user_data);

// Timer callback signature — matches map-server TimerFunc.
typedef int32_t (*plugin_timer_func)(int32_t tid, int64_t tick,
                                     int32_t id, intptr_t data);

// ============================================================
// Server function API sub-structs
// ============================================================

// ---- Script state helpers ----
struct plugin_script_api_t {
	bool        (*hasdata)(struct script_state* st, int n);
	int64_t     (*getnum) (struct script_state* st, int n);
	const char* (*getstr) (struct script_state* st, int n);
	void        (*pushint)(struct script_state* st, int64_t val);
	void        (*pushstr)(struct script_state* st, const char* val);
	struct map_session_data* (*rid2sd)(struct script_state* st);
};

// ---- Player (PC) functions ----
// All struct accessors let plugins read player data without including pc.hpp.
struct plugin_pc_api_t {
	// Send a plain text message to the player's chat window
	void (*message)(int32_t fd, const char* msg);

	// Add item(s) to the player's inventory.
	// log_type: LOG_TYPE_SCRIPT=7, LOG_TYPE_COMMAND=2, LOG_TYPE_NONE=0
	// Returns 0 on success.
	int  (*additem)(struct map_session_data* sd, const struct plugin_item_t* it,
	                int32_t amount, int log_type);

	// Remove item(s) from inventory slot n.
	char (*delitem)(struct map_session_data* sd, int32_t n,
	                int32_t amount, int32_t type, int16_t reason, int log_type);

	// Grant base and/or job EXP.
	void (*gainexp)(struct map_session_data* sd, struct block_list* src,
	                uint64_t base_exp, uint64_t job_exp, uint8_t exp_flag);

	char (*payzeny)(struct map_session_data* sd, int32_t zeny, int log_type);
	char (*getzeny)(struct map_session_data* sd, int32_t zeny, int log_type);

	// Warp — clrtype: CLR_TELEPORT=0, CLR_RESPAWN=1, CLR_OUTSIGHT=3
	int  (*setpos)(struct map_session_data* sd, uint16_t mapindex,
	               int32_t x, int32_t y, int clrtype);

	// Data accessors — use these instead of including pc.hpp / mmo.hpp.
	int32_t            (*get_fd)    (struct map_session_data* sd);
	int32_t            (*get_aid)   (struct map_session_data* sd);  // account_id
	const char*        (*get_name)  (struct map_session_data* sd);  // char name
	uint32_t           (*get_blv)   (struct map_session_data* sd);  // base_level
	uint32_t           (*get_jlv)   (struct map_session_data* sd);  // job_level
	int16_t            (*get_mapid) (struct map_session_data* sd);
	int16_t            (*get_pos_x) (struct map_session_data* sd);
	int16_t            (*get_pos_y) (struct map_session_data* sd);
	struct block_list* (*as_bl)     (struct map_session_data* sd);
};

// ---- Monster (mob) functions ----
struct plugin_mob_api_t {
	// Spawn monsters — size: 0=normal, 1=small, 2=big; ai: 0=normal mob
	// Returns number of mobs spawned.
	int32_t (*once_spawn)(struct map_session_data* sd,
	                      int16_t m, int16_t x, int16_t y,
	                      const char* mobname, int32_t mob_id, int32_t amount,
	                      const char* event, uint32_t size, int ai);

	// mob_data accessors — use these instead of including mob.hpp directly.
	int32_t     (*get_id)  (struct mob_data* md);
	int16_t     (*get_x)   (struct mob_data* md);
	int16_t     (*get_y)   (struct mob_data* md);
	const char* (*get_name)(struct mob_data* md);  // display name (jname)
};

// ---- Map / player lookup utilities ----
struct plugin_map_api_t {
	uint16_t    (*name2id)(const char* mapname);
	const char* (*id2name)(uint16_t mapindex);
	struct map_session_data* (*id2sd)    (int32_t id);
	struct map_session_data* (*charid2sd)(int32_t charid);
	struct map_session_data* (*nick2sd)  (const char* nick, bool allow_partial);

	// Iterate every block_list in map `m` whose type bit is set in `type_mask`
	// (e.g. PLUGIN_BL_PC=4 means pass `1<<4=16` for players, or use BL_ALL=0x1ff).
	// Returns the sum of cb return values.
	int32_t (*foreachinmap) (plugin_blcb cb, void* user, int16_t m, int32_t type_mask);

	// Iterate every block_list in the rectangle [(x0,y0),(x1,y1)] on map `m`.
	int32_t (*foreachinarea)(plugin_blcb cb, void* user, int16_t m,
	                         int16_t x0, int16_t y0, int16_t x1, int16_t y1,
	                         int32_t type_mask);

	// Read a mapflag (e_mapflag) for map `m`. Returns the flag value or 0.
	int32_t (*get_mapflag)(int16_t m, int32_t flag);
};

// ---- Status (HP / SP manipulation) ----
struct plugin_status_api_t {
	// flag: 1=no animation, 2=allow exceeding max
	int32_t (*heal)  (struct block_list* bl, int64_t hp, int64_t sp, int32_t flag);

	// walkdelay: delay after hit (ms); flag: 1=no death; skill_id: 0=generic
	int32_t (*damage)(struct block_list* src, struct block_list* target,
	                  int64_t hp, int64_t sp, int64_t walkdelay,
	                  int32_t flag, uint16_t skill_id);
};

// ---- Block list entity utilities ----
// Use these to inspect entity types and cross-cast pointers.
struct plugin_bl_api_t {
	// Returns the entity type (compare against e_plugin_bl_type values).
	int32_t (*get_type)(struct block_list* bl);

	// Returns sd if bl is a player (PLUGIN_BL_PC), null otherwise.
	struct map_session_data* (*as_sd)(struct block_list* bl);
};

// ---- Item data accessors ----
struct plugin_item_api_t {
	uint32_t (*get_nameid)(struct item* it);

	// item_db lookups (use these instead of including itemdb.hpp).
	bool        (*db_exists)   (uint32_t nameid);
	const char* (*db_get_name) (uint32_t nameid);  // internal name (e.g. "Apple")
	const char* (*db_get_ename)(uint32_t nameid);  // display name (e.g. "Apple")
	int32_t     (*db_get_type) (uint32_t nameid);  // IT_HEALING=0, IT_USABLE=2, IT_ETC=3, ...
};

// ---- Custom @commands ----
struct plugin_atcmd_api_t {
	bool (*register_cmd)(const char* name, int level, plugin_atcmd_func func);
};

// ---- Quest management ----
struct plugin_quest_api_t {
	int32_t (*add)(struct map_session_data* sd, int32_t quest_id);
	// status: 0=Q_INACTIVE, 1=Q_ACTIVE, 2=Q_COMPLETE
	int32_t (*update_status)(struct map_session_data* sd, int32_t quest_id, int status);
	// type: 0=HAVEQUEST, 1=PLAYTIME, 2=HUNTING
	int32_t (*check)(const struct map_session_data* sd, int32_t quest_id, int type);
};

// ---- NPC event triggering ----
struct plugin_npc_api_t {
	// event_name format: "NpcExname::OnEventLabel"
	int (*event)(struct map_session_data* sd, const char* event_name, int ontouch);
};

// ---- Skill utilities ----
struct plugin_skill_api_t {
	int32_t (*get_lv)(struct map_session_data* sd, uint16_t skill_id);
	int32_t (*use_id)(struct map_session_data* sd, uint16_t skill_id,
	                  uint16_t skill_lv, int32_t target_id);

	// skill_db lookups (use these instead of including skill.hpp).
	const char* (*get_name)(uint16_t skill_id);  // AEGIS name (e.g. "MG_FIREBOLT")
	int32_t     (*get_inf) (uint16_t skill_id);  // INF_ATTACK_SKILL=1, INF_GROUND_SKILL=2, ...
	uint16_t    (*name2id) (const char* name);   // 0 if not found
};

// ---- Storage ----
struct plugin_storage_api_t {
	int32_t (*open)(struct map_session_data* sd);
};

// ---- Clif (client packet) helpers ----
// `target` values match enum send_target: ALL_CLIENT=0, ALL_SAMEMAP=1,
// AREA=2, AREA_WOS=3, SELF=24, etc. See clif.hpp for the full list.
struct plugin_clif_api_t {
	// Send a plain text line to the player's chat window (same as pc.message).
	void (*displaymessage)(int32_t fd, const char* msg);

	// Trigger an emotion bubble above an entity. emote: 0=ET_SURPRISE,
	// 1=ET_QUESTION, 2=ET_DELIGHT, ... see emotion_type in clif.hpp.
	void (*emotion)(struct block_list* bl, int32_t emote);

	// Play a special effect on/around an entity.
	// effect_id: see effect_db.yml; target: SELF/AREA/etc.
	void (*specialeffect)       (struct block_list* bl, int32_t effect_id, int32_t target);
	void (*specialeffect_single)(struct block_list* bl, int32_t effect_id, int32_t fd);

	// Show a progress bar above the player (color: 0xRRGGBB, seconds: duration).
	void (*progressbar)      (struct map_session_data* sd, uint32_t color, uint32_t seconds);
	void (*progressbar_abort)(struct map_session_data* sd);

	// Server-wide / map-wide announce. type: BC_DEFAULT=0x00 (yellow),
	// BC_BLUE=0x10, BC_WOE=0x20, etc. target: ALL_CLIENT=0, ALL_SAMEMAP=1, ...
	void (*broadcast)(struct block_list* bl, const char* msg,
	                  int32_t type, int32_t target);

	// Colored chat message (color: 0xRRGGBB).
	void (*messagecolor)(struct block_list* bl, uint32_t color, const char* msg,
	                     bool rgb2bgr, int32_t target);
};

// ---- Timer ----
// Times are millisecond ticks; use `gettick()` as a baseline.
struct plugin_timer_api_t {
	int64_t (*gettick)(void);

	// One-shot timer. `tick` is the absolute wake-up tick (gettick() + delay_ms).
	// Returns the timer id (use it with delete_timer).
	int32_t (*add_timer)(int64_t tick, plugin_timer_func func,
	                     int32_t id, intptr_t data);

	// Repeating timer with the given interval (ms).
	int32_t (*add_timer_interval)(int64_t tick, plugin_timer_func func,
	                              int32_t id, intptr_t data, int32_t interval_ms);

	// Cancel a timer; pass the same `func` you registered with.
	int32_t (*delete_timer)(int32_t tid, plugin_timer_func func);
};

// ---- Logging (ShowXxx wrappers) ----
// Plugins must format their own strings (e.g. via snprintf) before calling —
// variadic format strings are not safe across DLL boundaries.
struct plugin_log_api_t {
	void (*info)   (const char* msg);
	void (*status) (const char* msg);
	void (*warning)(const char* msg);
	void (*error)  (const char* msg);
	void (*debug)  (const char* msg);
};

// ============================================================
// Main plugin API struct — passed to plugin_init()
// ============================================================
struct plugin_api_t {
	void (*hook_add)   (int hook_type, plugin_hook_cb cb, void* user_data, int priority);
	void (*hook_remove)(int hook_type, plugin_hook_cb cb);
	bool (*script_addcommand)(const char* name, const char* arg, plugin_script_func func);

	struct plugin_script_api_t  script;
	struct plugin_pc_api_t      pc;
	struct plugin_mob_api_t     mob;
	struct plugin_map_api_t     map;
	struct plugin_status_api_t  status;
	struct plugin_bl_api_t      bl;
	struct plugin_item_api_t    item_api;
	struct plugin_atcmd_api_t   atcmd;
	struct plugin_quest_api_t   quest;
	struct plugin_npc_api_t     npc;
	struct plugin_skill_api_t   skill;
	struct plugin_storage_api_t storage;
	struct plugin_clif_api_t    clif;
	struct plugin_timer_api_t   timer;
	struct plugin_log_api_t     log;
};

// ---- Plugin metadata ----
struct plugin_info_t {
	const char* name;
	const char* author;
	const char* version;
	const char* description;
};

/*
 * Every plugin DLL/SO must export these three symbols with C linkage:
 *
 *   PLUGIN_API plugin_info_t* plugin_info();
 *   PLUGIN_API bool plugin_init(plugin_api_t* api);
 *   PLUGIN_API void plugin_final();
 */

// ---- Server-side internal functions (not for plugin use) ----

void        plugin_manager_init(void);
void        plugin_manager_final(void);
int         plugin_hook_fire(int hook_type, void* data);
const char* plugin_get_cmd_arg(int idx);
bool        script_plugin_register(const char* name, const char* arg,
                                   plugin_script_func func, int cmd_idx);

#endif // MAP_PLUGIN_HPP
