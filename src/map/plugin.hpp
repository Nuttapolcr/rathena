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

	// Inter-server
	HOOK_INTIF_CONNECTED,   // map-server finished (re)connecting to the char-server

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

// HOOK_INTIF_CONNECTED — informational. Fires once per (re)connection to the
// char-server, after the inter-server tables (storage list, etc.) have been
// received. A good place to re-apply runtime registrations that the
// char-server overwrites on reconnect (see storage.define). HOOK_STOP is
// ignored.
struct plugin_intif_connected_t {
	bool first;   // true on the initial connect, false on a later reconnect
};

// ============================================================
// Callback types
// ============================================================

// All plugin callbacks accept a `user_data` pointer that is passed
// verbatim from the matching register*() call. It lets a single C
// function back multiple registrations without resorting to globals.

typedef int (*plugin_hook_cb)(void* data, void* user_data);
typedef int32_t (*plugin_script_func)(struct script_state* st, void* user_data);
typedef int32_t (*plugin_atcmd_func)(struct map_session_data* sd,
                                     const char* command,
                                     const char* message,
                                     void* user_data);

// Block-list iteration callback (for map.foreachinmap / foreachinarea).
// Return value is summed by the caller; non-zero typically indicates "matched".
typedef int32_t (*plugin_blcb)(struct block_list* bl, void* user_data);

// Timer callback signature — matches map-server TimerFunc.
// `data` is the closure pointer you passed to timer.add_timer.
typedef int32_t (*plugin_timer_func)(int32_t tid, int64_t tick,
                                     int32_t id, intptr_t data);

// Custom client packet handler. Called when a packet whose id was
// registered via packet.register_handler arrives on `fd`. `sd` is the
// player session (may be null before login, e.g. for handshake packets).
// Read payload via packet.read_b/w/l/str at offsets you define.
typedef void (*plugin_packet_func)(int32_t fd, struct map_session_data* sd,
                                   void* user_data);

// Result of a packet filter: PASS lets the engine's own handler run
// afterwards; STOP suppresses it (the plugin has consumed the packet).
#define PLUGIN_PACKET_PASS 0
#define PLUGIN_PACKET_STOP 1

// Filter callback for an incoming client packet. Installed for any cmd
// (including ones the engine already handles via clif_parse_*) and runs
// *before* the engine handler. Use packet.read_b/w/l/str/rest to inspect
// the payload (offsets from the cmd word). Return PLUGIN_PACKET_PASS or
// PLUGIN_PACKET_STOP. `sd` may be null for pre-login packets.
typedef int32_t (*plugin_packet_filter_func)(int32_t fd, struct map_session_data* sd,
                                             void* user_data);

// ---- Plugin status changes (SC) ----

// Calc-flag bits: which stats a plugin SC influences. When the SC
// starts or ends, the engine recalculates these stats on the entity
// and runs the SC's calc callback for each one. Base-stat bits cascade
// automatically (e.g. PLUGIN_SCB_STR also recalculates batk/matk).
// Mirrors a subset of e_scb_flag in status.hpp.
enum e_plugin_scb : int32_t {
	PLUGIN_SCB_STR   = 1 << 0,
	PLUGIN_SCB_AGI   = 1 << 1,
	PLUGIN_SCB_VIT   = 1 << 2,
	PLUGIN_SCB_INT   = 1 << 3,
	PLUGIN_SCB_DEX   = 1 << 4,
	PLUGIN_SCB_LUK   = 1 << 5,
	PLUGIN_SCB_MAXHP = 1 << 6,
	PLUGIN_SCB_MAXSP = 1 << 7,
	PLUGIN_SCB_SPEED = 1 << 8,
};

// Calc callback for a plugin SC. Invoked once per affected stat during
// status recalculation. `scb_kind` is a single PLUGIN_SCB_* bit telling
// you which stat is being computed; `cur_value` is the value the engine
// has so far; return the new value (do `cur_value + N`, `cur_value * f`,
// caps, etc.). `val1..val4` are the parameters passed to sc.start().
typedef int32_t (*plugin_sc_calc_func)(struct block_list* bl,
                                       int32_t sc_id, int32_t scb_kind,
                                       int32_t cur_value,
                                       int32_t val1, int32_t val2,
                                       int32_t val3, int32_t val4,
                                       void* user_data);

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

	// Suspend the running script. Call from inside a plugin script command,
	// then return PLUGIN_SCRIPT_CMD_SUCCESS — the script is parked at the
	// instruction after the command, with the player still attached to the
	// NPC. Returns an opaque token to use with `resume()` later.
	//
	// To pass a value back to the calling script, push it via pushint/pushstr
	// BEFORE returning from the script command (i.e. before resume()).
	//
	// The token is invalidated automatically if the script is freed
	// (player logout, NPC reload, server shutdown). Calling resume() on
	// such a token is a safe no-op.
	void* (*suspend)(struct script_state* st);

	// Resume a previously-suspended script. No-op if the token is null,
	// has already been resumed, or refers to a freed script state.
	void  (*resume)(void* token);

	// ---- Variable storage (setd / getd / setarray equivalents) ----
	//
	// Read or write a player/server script variable directly from C.
	// `varname` includes the prefix that selects the scope:
	//
	//     .       NPC scope          (lifetime: NPC instance)
	//     .@      scope/local        (lifetime: this stack frame)
	//     #       char-shared        (per-account, char-server stored)
	//     ##      account-wide       (per-account, login-server stored)
	//     @       temporary char     (player-bound, cleared on logout)
	//     $       global permanent
	//     $@      global temporary
	//     '       instance-scoped
	//
	// `index` is the array index (use 0 for non-array vars).
	// `sd` is the player whose vars are touched; it may be null only
	// for global ($) variables.
	void        (*set_var_num)(struct script_state* st, struct map_session_data* sd,
	                          const char* varname, int32_t index, int64_t value);
	void        (*set_var_str)(struct script_state* st, struct map_session_data* sd,
	                          const char* varname, int32_t index, const char* value);
	int64_t     (*get_var_num)(struct script_state* st, struct map_session_data* sd,
	                          const char* varname, int32_t index);
	const char* (*get_var_str)(struct script_state* st, struct map_session_data* sd,
	                          const char* varname, int32_t index);
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

	// ---- Stat bonuses (status_bonus_*) ----
	// Apply a bonus modifier to a player. `type` is one of the SP_*
	// values defined in script_constants.hpp (SP_MAXHP=6, SP_STR=13, ...).
	// These mirror pc_bonus / pc_bonus2 / pc_bonus3 / pc_bonus4 / pc_bonus5.
	void (*bonus) (struct map_session_data* sd, int32_t type, int32_t val);
	void (*bonus2)(struct map_session_data* sd, int32_t type,
	               int32_t val1, int32_t val2);
	void (*bonus3)(struct map_session_data* sd, int32_t type,
	               int32_t val1, int32_t val2, int32_t val3);
	void (*bonus4)(struct map_session_data* sd, int32_t type,
	               int32_t val1, int32_t val2, int32_t val3, int32_t val4);
	void (*bonus5)(struct map_session_data* sd, int32_t type,
	               int32_t val1, int32_t val2, int32_t val3, int32_t val4,
	               int32_t val5);

	// ---- Inventory / equip / parameter accessors ----

	// Sum every stack of `nameid` in the player's main inventory.
	int32_t (*countitem)(struct map_session_data* sd, uint32_t nameid);

	// Read a player parameter (Str=13, Agi=14, Vit=15, Int=16, Dex=17,
	// Luk=18, MaxHp=6, MaxSp=8, ...). See script_constants.hpp for SP_*.
	int64_t (*read_param)(struct map_session_data* sd, int32_t type);

	// nameid of the item currently equipped in `equip_index`
	// (EQI_HEAD_TOP=0 through EQI_MAX-1). Returns 0 if no item.
	uint32_t (*get_equip_nameid)(struct map_session_data* sd, int32_t equip_index);

	// ---- NPC dialog response (read after scriptmenu/scriptinput) ----
	int32_t     (*get_npc_id)    (struct map_session_data* sd);  // current NPC bl id
	int32_t     (*get_npc_menu)  (struct map_session_data* sd);  // last selection (1-based)
	int32_t     (*get_npc_amount)(struct map_session_data* sd);  // integer input
	const char* (*get_npc_str)   (struct map_session_data* sd);  // string input
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

// ---- Status (HP / SP manipulation, status changes) ----
struct plugin_status_api_t {
	// flag: 1=no animation, 2=allow exceeding max
	int32_t (*heal)  (struct block_list* bl, int64_t hp, int64_t sp, int32_t flag);

	// walkdelay: delay after hit (ms); flag: 1=no death; skill_id: 0=generic
	int32_t (*damage)(struct block_list* src, struct block_list* target,
	                  int64_t hp, int64_t sp, int64_t walkdelay,
	                  int32_t flag, uint16_t skill_id);

	// ---- Status changes (SC_*) ----
	//
	// `type` is an sc_type value. Pass a literal number or resolve a name
	// via sc_id() below. `rate` is 0..10000 (10000 = always). `flag` is a
	// bitmask of SCSTART_*: NOAVOID=0x01, NOTICKDEF=0x02, LOADED=0x04,
	// NORATEDEF=0x08, NOICON=0x10; 0 for the usual behaviour.
	// Returns true if the status was applied.
	bool    (*change_start)(struct block_list* src, struct block_list* bl,
	                        int32_t type, int32_t rate,
	                        int32_t val1, int32_t val2, int32_t val3, int32_t val4,
	                        int64_t duration_ms, int32_t flag);

	// End one active status. Returns 1 if a status was actually removed.
	int32_t (*change_end)(struct block_list* bl, int32_t type);

	// Clear active statuses. type: 0 = everything (including permanent),
	// 1 = the normal removable set (matches status_change_clear).
	void    (*change_clear)(struct block_list* bl, int32_t type);

	// True if `type` is currently active on `bl`.
	bool    (*has_change)(struct block_list* bl, int32_t type);

	// Read val1..val4 of an active status (`which` is 1..4). Returns 0 if
	// the status isn't active or `which` is out of range.
	int32_t (*change_val)(struct block_list* bl, int32_t type, int32_t which);

	// Resolve an SC constant name to its sc_type value. Accepts the bare
	// name ("FREEZE") or the full constant ("SC_FREEZE"). Returns SC_NONE
	// (-1) if neither resolves.
	int32_t (*sc_id)(const char* name);
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
	bool (*register_cmd)(const char* name, int level,
	                     plugin_atcmd_func func, void* user_data);
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

	// Append a .txt script file to the engine's source-file list. The file
	// is parsed during do_init_npc, so this only does anything useful if
	// called from plugin_init() (before NPC compilation runs). `path` is
	// resolved relative to the map-server's working directory — typically
	// the rAthena root. Returns false if the file cannot be opened or if
	// it is already queued.
	bool (*add_script_file)(const char* path);

	// Remove a .txt path from the engine's source-file list. Use this to
	// suppress entries that scripts_main.conf added but a mod wants to
	// replace or disable. Has to be called from plugin_init() to take
	// effect — once do_init_npc has run, scripts are already parsed.
	// Returns true if the path was present before the call.
	bool (*del_script_file)(const char* path);
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
// `mode` for open_premium is an OR of e_storage_mode bits: STOR_MODE_GET=0x1
// (take only), STOR_MODE_PUT=0x2 (deposit only), STOR_MODE_ALL=0x3 (both).
struct plugin_storage_api_t {
	// Personal (Kafra) storage. Returns 0 on success, non-zero if it could
	// not be opened (already open, intimacy lock, etc.).
	int32_t (*open)(struct map_session_data* sd);

	// Guild storage. Returns 0 on success, non-zero on failure (no guild,
	// already in use by another member, GMs blocked, …).
	int32_t (*open_guild)(struct map_session_data* sd);

	// Premium / extended storage configured in storage.yml. `storage_id`
	// must be a known id (see exists()); `mode` limits get/put access.
	// Returns true if the load was kicked off, false on a bad id / when one
	// is already loading.
	bool (*open_premium)(struct map_session_data* sd, int32_t storage_id, int32_t mode);

	// True if `storage_id` is a configured premium storage.
	bool (*exists)(int32_t storage_id);

	// Register (or update) a storage definition in the map-server's table —
	// the same table storage.yml feeds. The client distinguishes storage
	// tabs by this `name` (sent in ZC_INVENTORY_START as the INVTYPE_STORAGE
	// label), so registering a fresh `id`/`name` and opening it gives the
	// player a new storage window.
	//   id        — 0 is the personal (Kafra) storage; registering it just
	//               renames that tab. 1..255 is a premium/extended storage
	//               that open_premium() can open.
	//   name      — client-visible tab title (truncated to NAME_LENGTH-1).
	//   sql_table — char-server table that backs it; null/empty keeps the
	//               existing one (or defaults to "storage" for a new id).
	//   max_num   — slot cap, clamped to MAX_STORAGE; 0 keeps the default.
	// Returns false on a bad id or empty name.
	//
	// NOTE: this changes only the *map* server. For a premium storage's
	// items to actually load and save, the char-server must know the same
	// id/table — list it in db/(pre-)re/storage.yml. The char-server resends
	// its storage list on (re)connect, which overwrites entries added here.
	bool (*define)(int32_t id, const char* name,
	               const char* sql_table, int32_t max_num);

	// Client-visible name of storage `id` ("Storage" if not registered).
	const char* (*get_name)(int32_t id);
};

// ---- Client UI windows ----
// Opens one of the built-in client panels. The generic open() maps to the
// ZC_UI_OPEN packet; `ui_type` is an out_ui_type value:
//   0 BANK   1 STYLIST   2 CAPTCHA   3 MACRO   5 TIP   6 QUEST
//   7 ATTENDANCE   8 ENCHANTGRADE   10 ENCHANT
// `data` is the per-panel payload (quest id for QUEST, tip id for TIP, …);
// pass 0 when the panel takes none. Panels not supported by the player's
// PACKETVER are silently ignored by the client/engine.
struct plugin_ui_api_t {
	void (*open)      (struct map_session_data* sd, int32_t ui_type, int32_t data);
	void (*dressroom) (struct map_session_data* sd);  // dress-room preview window
	void (*roulette)  (struct map_session_data* sd);  // roulette window (needs feature_roulette)
	void (*mail)      (struct map_session_data* sd);  // mailbox window
};

// ---- Battle config ----
// Read or override the conf/battle/*.conf settings at runtime. `name` is
// the setting key as written in those files (e.g. "base_exp_rate",
// "enable_pet_autofeed", "max_walk_speed").
struct plugin_battle_api_t {
	// Current integer value of `name`, or 0 if it isn't a known setting.
	// Many booleans are stored as 0/1; use has() to tell "0" from "unknown".
	int32_t (*get)(const char* name);

	// Set `name` to `value`. The engine clamps to the setting's declared
	// [min, max] (logging a warning and reverting to the default if out of
	// range, mirroring battle_config_read). Returns true if `name` is a
	// known setting. Changes are not written back to the .conf files.
	bool (*set)(const char* name, int32_t value);

	// True if `name` is a known battle setting.
	bool (*has)(const char* name);
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

	// ---- NPC script dialog ----
	// Build mes/next/menu/input flows from a plugin script command.
	// `oid` is the dialog-owning NPC bl id, normally `st->oid`.
	//
	// After scriptmenu / scriptinput / scriptinputstr the player's
	// response lands on the session — read it via the matching
	// pc.get_npc_menu / pc.get_npc_amount / pc.get_npc_str accessors
	// once the script resumes.
	void (*scriptmes)     (struct map_session_data* sd, uint32_t oid, const char* msg);
	void (*scriptnext)    (struct map_session_data* sd, uint32_t oid);
	void (*scriptclose)   (struct map_session_data* sd, uint32_t oid);
	void (*scriptmenu)    (struct map_session_data* sd, uint32_t oid, const char* menu);
	void (*scriptinput)   (struct map_session_data* sd, uint32_t oid);
	void (*scriptinputstr)(struct map_session_data* sd, uint32_t oid);
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

// ---- Plugin status changes ----
// Register custom status effects that participate in status.cpp's stat
// recalculation, carry up to four parameters, expire on a timer, and
// optionally show a client status icon.
struct plugin_sc_api_t {
	// Register a new plugin SC. Returns its id (>= 0) for use with the
	// other calls, or -1 on failure (bad args / out of slots).
	//   name      — diagnostic label.
	//   calc_flag — OR of e_plugin_scb bits; the stats this SC touches.
	//   icon      — EFST_* client status icon (0 = no icon).
	//   calc      — invoked during status recalculation per affected stat.
	//   user_data — passed unchanged to `calc`.
	int32_t (*register_sc)(const char* name, int32_t calc_flag, int32_t icon,
	                       plugin_sc_calc_func calc, void* user_data);

	// Start (or refresh) a plugin SC on `bl`. duration_ms <= 0 makes it
	// permanent (until end() or the entity is destroyed). Re-starting an
	// active SC replaces its val1..val4 and resets the timer. Triggers a
	// stat recalc for the SC's calc_flag. Returns true on success.
	bool (*start)(struct block_list* bl, int32_t sc_id,
	              int32_t val1, int32_t val2, int32_t val3, int32_t val4,
	              int64_t duration_ms);

	// End a plugin SC on `bl`. Triggers a stat recalc. Returns true if
	// it was active.
	bool (*end)(struct block_list* bl, int32_t sc_id);

	// Is the SC active on `bl`? If so and an out_ pointer is non-null,
	// fills it with the stored val1..val4.
	bool (*active)(struct block_list* bl, int32_t sc_id,
	               int32_t* out_val1, int32_t* out_val2,
	               int32_t* out_val3, int32_t* out_val4);
};

// ---- Custom client packets ----
// Lets plugins install handlers for previously-unused packet IDs, read the
// incoming buffer, and push outbound packets to clients. The packet ID
// space is 0x064..0xCFF; pick something not used by your client build.
struct plugin_packet_api_t {
	// Install a handler. `length` is the fixed packet size in bytes
	// (including the 2-byte cmd header), or -1 for variable-length
	// packets where the length is read from offset 2 (uint16). Returns
	// false if `cmd` is out of range or `func` is null. `user_data` is
	// passed unchanged to the handler on every dispatch.
	//
	// IMPORTANT: register handlers from plugin_init(). The plugin
	// system automatically clears them on plugin_final() so the
	// engine never calls into an unloaded DLL.
	bool (*register_handler)(uint16_t cmd, int16_t length,
	                         plugin_packet_func func, void* user_data);

	// Drop a previously-registered handler.
	bool (*unregister_handler)(uint16_t cmd);

	// Install a *filter* for ANY incoming packet id — including ones the
	// engine handles itself. The filter runs in clif_parse before the
	// engine's handler; returning PLUGIN_PACKET_STOP suppresses that
	// handler (the packet is otherwise still consumed/skipped from the
	// buffer as normal). One filter per cmd; re-registering replaces it.
	// `user_data` is forwarded to the filter unchanged.
	//
	// Like register_handler, install filters from plugin_init(); the
	// plugin system clears them on plugin_final().
	bool (*register_filter)(uint16_t cmd, plugin_packet_filter_func func, void* user_data);

	// Drop a previously-registered filter.
	bool (*unregister_filter)(uint16_t cmd);

	// Read primitives from the incoming packet (within a handler or filter).
	// `offset` is measured from the start of the packet (cmd at 0..1).
	uint8_t     (*read_b)  (int32_t fd, int32_t offset);
	uint16_t    (*read_w)  (int32_t fd, int32_t offset);
	uint32_t    (*read_l)  (int32_t fd, int32_t offset);
	const char* (*read_str)(int32_t fd, int32_t offset);  // pointer into recv buffer
	int32_t     (*read_rest)(int32_t fd);                 // bytes left in recv buffer

	// Push a fully-formed packet buffer (cmd at offset 0..1) to a
	// single fd. Use this for SELF-targeted custom packets.
	void (*send_self)(int32_t fd, const void* data, int32_t len);

	// Broadcast via clif_send. `target` matches enum send_target
	// (ALL_CLIENT=0, AREA=2, SELF=24, etc.). `bl` may be null only
	// when target==ALL_CLIENT.
	void (*send_target)(struct block_list* bl, const void* data,
	                    int32_t len, int32_t target);
};

// ============================================================
// Main plugin API struct — passed to plugin_init()
// ============================================================
struct plugin_api_t {
	void (*hook_add)   (int hook_type, plugin_hook_cb cb, void* user_data, int priority);
	void (*hook_remove)(int hook_type, plugin_hook_cb cb);
	bool (*script_addcommand)(const char* name, const char* arg,
	                          plugin_script_func func, void* user_data);

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
	struct plugin_ui_api_t      ui;
	struct plugin_clif_api_t    clif;
	struct plugin_timer_api_t   timer;
	struct plugin_log_api_t     log;
	struct plugin_packet_api_t  packet;
	struct plugin_sc_api_t      sc;
	struct plugin_battle_api_t  battle;
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
bool        script_plugin_register(const char* name, const char* arg, int cmd_idx);

// Server-internal: invoked by the script engine's plugin trampoline.
// Returns the value of the plugin's actual function. Defined in plugin.cpp.
int32_t     plugin_dispatch_script_cmd(int idx, struct script_state* st);

// Notify the plugin system that a script_state is about to be freed,
// so any plugin-held suspend tokens for it become no-ops on resume().
// Called from script_free_state() in script.cpp.
void        plugin_script_state_freed(struct script_state* st);

// Apply every active plugin SC on `bl` that affects `scb_kind` (one of
// the e_plugin_scb bits) to `cur_value`, returning the result.
// Called from status_calc_bl() in status.cpp.
int32_t     plugin_status_calc(struct block_list* bl, int32_t scb_kind, int32_t cur_value);

// Drop all active plugin SCs for `bl` (called from unit_free() so a
// recycled block-list id does not inherit stale effects).
void        plugin_sc_clear(struct block_list* bl);

// Run the plugin filter (if any) registered for `cmd` before the engine's
// own clif handler. Returns PLUGIN_PACKET_STOP when the engine handler
// should be skipped. Called from clif_parse() in clif.cpp.
int32_t     plugin_packet_filter(int32_t fd, uint16_t cmd, struct map_session_data* sd);

#endif // MAP_PLUGIN_HPP
