#include "modloader.hpp"
#include "db_store.hpp"
#include "lua_bridge.hpp"
#include "workshop.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace workshop {

ModLoader& ModLoader::instance() {
    static ModLoader inst;
    return inst;
}

// ---- helpers ----

static bool dir_exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

static bool file_exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

static std::vector<std::string> list_subdirs(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (entry.is_directory(ec)) out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::vector<std::string> read_string_seq(const YAML::Node& n) {
    std::vector<std::string> out;
    if (!n) return out;
    if (n.IsSequence()) {
        for (const auto& item : n) out.push_back(item.as<std::string>());
    } else if (n.IsScalar()) {
        out.push_back(n.as<std::string>());
    }
    return out;
}

static bool parse_modinfo(const std::string& mod_dir, ModInfo& mi) {
    std::string yml = mod_dir + "/modinfo.yml";
    if (!file_exists(yml)) {
        wlog_warning("skipping %s — no modinfo.yml", mod_dir.c_str());
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(yml);
    } catch (const std::exception& e) {
        wlog_error("YAML parse failed for %s: %s", yml.c_str(), e.what());
        return false;
    }

    // rAthena-style header is supported but optional; we accept either
    // a top-level "name" key or a `Header / Body` layout.
    YAML::Node body = root["Body"] ? root["Body"] : root;

    if (!body["name"]) {
        wlog_error("%s: missing required field 'name'", yml.c_str());
        return false;
    }
    mi.dir         = mod_dir;
    mi.name        = body["name"].as<std::string>();
    mi.version     = body["version"]     ? body["version"].as<std::string>()     : "0.0.0";
    mi.author      = body["author"]      ? body["author"].as<std::string>()      : "";
    mi.description = body["description"] ? body["description"].as<std::string>() : "";
    mi.scripts          = read_string_seq(body["scripts"]);
    mi.dependencies     = read_string_seq(body["dependencies"]);
    mi.rathena_scripts  = read_string_seq(body["rathena_scripts"]);
    mi.disable_scripts  = read_string_seq(body["disable_scripts"]);
    mi.load_order       = body["load_order"] ? body["load_order"].as<int>() : 100;
    mi.on_init          = body["on_init"]    ? body["on_init"].as<std::string>() : "";
    mi.enabled          = body["enabled"]    ? body["enabled"].as<bool>()      : true;

    // db: optional list of DB YAML files to register before scripts run.
    // Accepts shorthand and detailed forms:
    //   db:
    //     - db/items.yml                # path only; key=Id, override=replace
    //     - path: db/mobs.yml           # detailed
    //       key:      Id
    //       override: merge
    if (auto db = body["db"]) {
        if (db.IsSequence()) {
            for (auto e : db) {
                DbFile f;
                if (e.IsScalar()) {
                    f.path = e.as<std::string>();
                } else if (e.IsMap()) {
                    if (!e["path"]) {
                        wlog_warning("%s: db entry missing 'path'",
                                     yml.c_str());
                        continue;
                    }
                    f.path          = e["path"].as<std::string>();
                    f.key_field     = e["key"]
                        ? e["key"].as<std::string>() : "";
                    f.override_mode = e["override"]
                        ? e["override"].as<std::string>() : "";
                } else {
                    wlog_warning("%s: db entry is neither string nor map",
                                 yml.c_str());
                    continue;
                }
                mi.db_files.push_back(std::move(f));
            }
        } else {
            wlog_warning("%s: db section must be a sequence", yml.c_str());
        }
    }

    if (mi.scripts.empty()) {
        wlog_warning("%s: 'scripts' list is empty — mod will load nothing",
                     yml.c_str());
    }
    return true;
}

// Topological sort: dependencies before dependents, ties broken by
// load_order (lower = earlier), then by mod name.
static bool resolve_order(std::vector<ModInfo>& mods,
                          std::vector<ModInfo>& out) {
    std::map<std::string, ModInfo*> by_name;
    for (auto& m : mods) by_name[m.name] = &m;

    std::map<std::string, int> color; // 0=unseen 1=visiting 2=done
    std::vector<ModInfo*> sorted;
    bool ok = true;

    std::function<void(ModInfo*)> visit = [&](ModInfo* m) {
        if (!m) return;
        if (color[m->name] == 2) return;
        if (color[m->name] == 1) {
            wlog_error("dependency cycle involving mod '%s'", m->name.c_str());
            ok = false;
            return;
        }
        color[m->name] = 1;
        for (auto& dep : m->dependencies) {
            auto it = by_name.find(dep);
            if (it == by_name.end()) {
                wlog_error("mod '%s' depends on missing mod '%s'",
                           m->name.c_str(), dep.c_str());
                ok = false;
                continue;
            }
            visit(it->second);
        }
        color[m->name] = 2;
        sorted.push_back(m);
    };

    // Visit in (load_order, name) order so siblings without dependencies
    // keep a stable, intuitive sequence.
    std::vector<ModInfo*> roots;
    roots.reserve(mods.size());
    for (auto& m : mods) roots.push_back(&m);
    std::sort(roots.begin(), roots.end(), [](ModInfo* a, ModInfo* b) {
        if (a->load_order != b->load_order) return a->load_order < b->load_order;
        return a->name < b->name;
    });

    for (auto* m : roots) visit(m);

    out.clear();
    out.reserve(sorted.size());
    for (auto* m : sorted) out.push_back(*m);
    return ok;
}

// ---- discover / load ----

bool ModLoader::discover(const std::string& mods_dir) {
    ordered_.clear();
    std::vector<ModInfo> raw;

    for (auto& sub : list_subdirs(mods_dir)) {
        ModInfo mi;
        if (!parse_modinfo(mods_dir + "/" + sub, mi)) continue;
        if (!mi.enabled) {
            // Disabled mods are reported but excluded from dependency
            // resolution — anything that depends on them will surface as
            // a "missing dependency" error, which is the right signal.
            wlog_status("mod '%s' is disabled (enabled: false in modinfo.yml)",
                        mi.name.c_str());
            continue;
        }
        raw.push_back(std::move(mi));
    }

    if (raw.empty()) {
        wlog_status("no enabled mods found in %s", mods_dir.c_str());
        return true;
    }

    return resolve_order(raw, ordered_);
}

bool ModLoader::load_all() {
    bool all_ok = true;
    auto& bridge = LuaBridge::instance();

    for (const auto& m : ordered_) {
        wlog_status("loading mod '%s' v%s", m.name.c_str(), m.version.c_str());
        bool mod_ok = true;

        // DB files load FIRST so that mod scripts can call db_get/db_each
        // at top level. Within a mod the listed order matters; across
        // mods, dependencies run before dependents (resolve_order took
        // care of that).
        for (const auto& f : m.db_files) {
            std::string path = m.dir + "/" + f.path;
            if (!file_exists(path)) {
                wlog_warning("  db file not found: %s", path.c_str());
                mod_ok = false;
                continue;
            }
            if (!DbStore::instance().load_file(
                    path, m.name, f.key_field,
                    parse_override_mode(f.override_mode))) {
                mod_ok = false;
            }
        }

        // disable_scripts removes entries that scripts_main.conf already
        // queued via map_config_read. Must run before do_init_npc parses
        // the queue — i.e. while plugin_init is still on the stack.
        for (const auto& s : m.disable_scripts) {
            if (g_api->npc.del_script_file(s.c_str())) {
                wlog_status("  disabled script: %s", s.c_str());
            } else {
                wlog_warning("  disable_scripts: '%s' was not in the queue",
                             s.c_str());
            }
        }

        // rathena_scripts adds .txt files to the engine's source list.
        // The path stored in the queue is the same one the engine reads
        // when it calls npc_parsesrcfile, so it must be relative to the
        // map-server's cwd (the rAthena root).
        for (const auto& s : m.rathena_scripts) {
            std::string path = m.dir + "/" + s;
            if (!file_exists(path)) {
                wlog_warning("  rathena_script not found: %s", path.c_str());
                mod_ok = false;
                continue;
            }
            if (g_api->npc.add_script_file(path.c_str())) {
                wlog_status("  queued rathena script: %s", path.c_str());
            } else {
                wlog_warning("  could not queue rathena script: %s",
                             path.c_str());
                mod_ok = false;
            }
        }

        for (const auto& s : m.scripts) {
            std::string path = m.dir + "/" + s;
            if (!file_exists(path)) {
                wlog_warning("  script not found: %s", path.c_str());
                mod_ok = false;
                continue;
            }
            if (!bridge.run_file(path, m.name + "/" + s)) {
                wlog_error("  %s: %s", path.c_str(),
                           bridge.last_error().c_str());
                mod_ok = false;
            }
        }
        if (mod_ok && !m.on_init.empty()) {
            if (!bridge.call_global(m.on_init.c_str())) {
                wlog_warning("  on_init '%s' failed: %s",
                             m.on_init.c_str(), bridge.last_error().c_str());
                mod_ok = false;
            }
        }
        all_ok = all_ok && mod_ok;
    }
    return all_ok;
}

// ---- scaffold ----

static bool write_text(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return true;
}

bool scaffold_mods_dir(const std::string& mods_dir) {
    std::string ex = mods_dir + "/example";
    if (dir_exists(ex)) return false;

    std::error_code ec;
    if (!std::filesystem::create_directories(ex, ec)) return false;
    std::string scripts = ex + "/scripts";
    std::filesystem::create_directories(scripts, ec);
    std::string db_dir = ex + "/db";
    std::filesystem::create_directories(db_dir, ec);

    std::string modinfo =
        "# modinfo.yml — describes a workshop mod.\n"
        "#\n"
        "# Required:  name, scripts\n"
        "# Optional:  enabled (default true), version, author, description,\n"
        "#            dependencies, load_order, on_init, db,\n"
        "#            rathena_scripts, disable_scripts\n"
        "\n"
        "name: example\n"
        "# Disabled by default — flip to true (or remove this line) to load it.\n"
        "enabled: false\n"
        "version: 0.1.0\n"
        "author: rAthena Workshop\n"
        "description: Example mod scaffolded automatically on first run.\n"
        "load_order: 100\n"
        "scripts:\n"
        "  - scripts/hello.lua\n"
        "# db: rAthena-style YAML files. Each entry can be a bare path\n"
        "# (key=Id, override=replace) or a map with explicit options.\n"
        "#\n"
        "# `key` picks the primary-key field — default 'Id' for item/mob\n"
        "# style files, but status.yml is keyed by its 'Status' name.\n"
        "#\n"
        "# Override modes when a key collides with an earlier mod:\n"
        "#   replace  — newer wins (default; matches db/import/ behaviour)\n"
        "#   skip     — first-seen wins\n"
        "#   error    — log + skip\n"
        "#   merge    — deep-merge YAML maps key by key\n"
        "db:\n"
        "  - db/sample_items.yml\n"
        "  - path: db/sample_status.yml\n"
        "    key:  Status\n"
        "#  - path: ../../../db/re/status.yml   # the real rAthena status DB\n"
        "#    key:  Status\n"
        "# rathena_scripts: classic .txt NPC files queued for the engine.\n"
        "# Paths are relative to this mod's folder and are picked up by\n"
        "# do_init_npc just like entries from scripts_main.conf.\n"
        "rathena_scripts:\n"
        "  - npc/example_npc.txt\n"
        "# disable_scripts: paths to drop from the engine's queue. Use the\n"
        "# exact string scripts_main.conf wrote (relative to rAthena root).\n"
        "# disable_scripts:\n"
        "#   - npc/airports/airships.txt\n"
        "on_init: example_on_init\n"
        "# dependencies:\n"
        "#   - other_mod_name\n";
    write_text(ex + "/modinfo.yml", modinfo);

    std::string npc_dir = ex + "/npc";
    std::filesystem::create_directories(npc_dir, ec);
    std::string example_npc =
        "// example/npc/example_npc.txt — classic rAthena NPC script,\n"
        "// queued for the engine via modinfo.yml's rathena_scripts.\n"
        "//\n"
        "// Click the NPC at prontera (150,150) to try the workshop\n"
        "// dialog buildins (shop_intro / shop_handle).\n"
        "\n"
        "prontera,150,150,5\tscript\tWorkshop Demo\t100,{\n"
        "\tshop_intro;\n"
        "\tshop_handle;\n"
        "\tend;\n"
        "}\n";
    write_text(npc_dir + "/example_npc.txt", example_npc);

    // Sample DB file — same Header/Body shape rAthena uses, so users can
    // copy real item entries from db/<region>/item_db.yml without editing.
    std::string sample_items =
        "# example/db/sample_items.yml — sample workshop DB.\n"
        "# Header.Type is the bucket name passed to db_get/db_each in Lua.\n"
        "# Override behaviour is set per-file in modinfo.yml's db: section.\n"
        "Header:\n"
        "  Type: ITEM_DB\n"
        "  Version: 1\n"
        "Body:\n"
        "  - Id: 90001\n"
        "    AegisName: Workshop_Token\n"
        "    Name: Workshop Token\n"
        "    Type: Etc\n"
        "    Buy: 0\n"
        "    Weight: 0\n"
        "  - Id: 90002\n"
        "    AegisName: Workshop_Apple\n"
        "    Name: Workshop Apple\n"
        "    Type: Healing\n"
        "    Buy: 1\n"
        "    Weight: 1\n"
        "    Heal: 25\n";
    write_text(db_dir + "/sample_items.yml", sample_items);

    // Status-keyed sample — mirrors db/re/status.yml's shape (entries are
    // keyed by the `Status:` name, not a numeric Id). modinfo.yml above
    // declares `key: Status` so the loader indexes on the right field.
    std::string sample_status =
        "# example/db/sample_status.yml — Status-keyed workshop DB.\n"
        "# Same shape as rAthena's db/re/status.yml; query in Lua with the\n"
        "# name string: db_get('STATUS_DB', 'Stone').\n"
        "Header:\n"
        "  Type: STATUS_DB\n"
        "  Version: 1\n"
        "Body:\n"
        "  - Status: Stone\n"
        "    DurationLookup: NPC_PETRIFYATTACK\n"
        "  - Status: Freeze\n"
        "    DurationLookup: MG_FROSTDIVER\n";
    write_text(db_dir + "/sample_status.yml", sample_status);

    std::string hello =
        "-- example/scripts/hello.lua\n"
        "-- Loaded by the workshop plugin on map-server startup.\n"
        "\n"
        "log_status('hello from example mod!')\n"
        "\n"
        "function example_on_init()\n"
        "    log_status('example_on_init: ' .. tostring(gettick()) .. ' tick')\n"
        "    start_timer('demo_loop')        -- begin the OnTimer demo\n"
        "end\n"
        "\n"
        "-- React to a player logging in.\n"
        "hook('pc_login', function(ctx)\n"
        "    if ctx.player then\n"
        "        message(ctx.player, 'Welcome back, ' .. ctx.player.name .. '!')\n"
        "    end\n"
        "end)\n"
        "\n"
        "-- Register a custom NPC script command, callable from any .txt npc:\n"
        "--   workshop_pinkpoke 1, 2;\n"
        "register_buildin('workshop_pinkpoke', 'ii',\n"
        "    function(player, a, b)\n"
        "        log_info('workshop_pinkpoke ' .. a .. ' ' .. b)\n"
        "        if player then\n"
        "            announce('pinkpoke from ' .. player.name)\n"
        "        end\n"
        "        return a + b\n"
        "    end)\n"
        "\n"
        "-- Async demo. NPC usage:\n"
        "--   .@n = workshop_async_roll(6);  // pauses ~1s then yields 1..6\n"
        "register_buildin('workshop_async_roll', 'i', function(player, max)\n"
        "    if max < 1 then max = 1 end\n"
        "    sleep(1000)              -- script_state is parked here\n"
        "    return math.random(1, max)\n"
        "end)\n"
        "\n"
        "-- Register an @command. Players use:  @hello\n"
        "register_atcmd('hello', 0, function(player, args)\n"
        "    if player then\n"
        "        message(player, 'Hello, ' .. player.name .. '!')\n"
        "    end\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- Clock events — same names as rAthena (OnClock/OnMinute/OnHour/OnDay).\n"
        "-- Use the rAthena label format directly ...\n"
        "on_event('OnMinute00', function(name)\n"
        "    log_status('clock event ' .. name .. ' fired')\n"
        "end)\n"
        "-- ... or the typed shortcuts:\n"
        "on_clock(13, 0, function() announce('It is 1pm server time!') end)\n"
        "on_hour(0,    function() log_status('midnight tick') end)\n"
        "on_day(1, 1,  function() announce('Happy new year!') end)\n"
        "\n"
        "-- Named relative timers (mirror NPC OnTimer<ms>).\n"
        "-- The first arg of the handler is the timer name, the second is the\n"
        "-- offset that fired — handy when one function handles many ticks.\n"
        "on_timer('demo_loop',  5000, function() log_status('demo_loop: 5s') end)\n"
        "on_timer('demo_loop', 10000, function(name)\n"
        "    log_status(name .. ': 10s, restarting')\n"
        "    init_timer(name)\n"
        "    start_timer(name)\n"
        "end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- NPC dialog demo (multi-step shop)\n"
        "-- ----------------------------------------------------------------\n"
        "-- Dialog primitives (next_dialog/menu/input_*) park the calling\n"
        "-- script and engine resumes it after the player responds — the\n"
        "-- code AFTER those calls in this Lua function does NOT run. The\n"
        "-- continuation lives in the next NPC script command (here:\n"
        "-- shop_handle).\n"
        "--\n"
        "-- NPC usage:\n"
        "--   prontera,150,150,5\\tscript\\tShop\\t100,{\n"
        "--       shop_intro;\n"
        "--       shop_handle;\n"
        "--       end;\n"
        "--   }\n"
        "register_buildin('shop_intro', '', function(player)\n"
        "    -- Persistent visit counter using a char-shared variable (#).\n"
        "    local visits = get_var(player, '#workshop_visits') + 1\n"
        "    set_var(player, '#workshop_visits', visits)\n"
        "\n"
        "    mes(player, 'Welcome, ' .. player.name .. '!')\n"
        "    mes(player, 'You have visited ' .. visits .. ' time(s).')\n"
        "    mes(player, 'Apples in your bag: ' .. countitem(player, 512))\n"
        "    menu(player, 'Get an apple:Buff me (+10 Str):Just leave')\n"
        "    -- script suspended; flow continues in shop_handle below.\n"
        "end)\n"
        "\n"
        "register_buildin('shop_handle', '', function(player)\n"
        "    local choice = npc_menu(player)\n"
        "    if choice == 1 then\n"
        "        getitem(player, 512, 1)        -- nameid 512 = Apple\n"
        "        mes(player, 'Here is your apple.')\n"
        "    elseif choice == 2 then\n"
        "        local before = read_param(player, 13)  -- SP_STR = 13\n"
        "        bonus2(player, 13, 10, 0)              -- session-only Str+10\n"
        "        local after  = read_param(player, 13)\n"
        "        mes(player, 'Str ' .. before .. ' -> ' .. after)\n"
        "    else\n"
        "        mes(player, 'See you next time!')\n"
        "    end\n"
        "    close_dialog(player)\n"
        "end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- input_int demo — ask for a number, branch on the response.\n"
        "-- ----------------------------------------------------------------\n"
        "-- NPC usage:\n"
        "--   ask_age;        // pops the input prompt\n"
        "--   ask_age_handle; // reads npc_amount() and replies\n"
        "register_buildin('ask_age', '', function(player)\n"
        "    mes(player, 'How old are you?')\n"
        "    input_int(player)                  -- script parked\n"
        "end)\n"
        "\n"
        "register_buildin('ask_age_handle', '', function(player)\n"
        "    local age = npc_amount(player)\n"
        "    if age < 18 then\n"
        "        mes(player, 'Quite young at ' .. age .. '!')\n"
        "    else\n"
        "        mes(player, 'Welcome, ' .. age .. '-year-old.')\n"
        "    end\n"
        "    close_dialog(player)\n"
        "end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- Workshop DB demo (db/sample_items.yml is registered in modinfo)\n"
        "-- ----------------------------------------------------------------\n"
        "-- Top-level code can already query the DB because the loader runs\n"
        "-- db files BEFORE scripts within the same mod load order.\n"
        "log_status('ITEM_DB count = ' .. db_count('ITEM_DB'))\n"
        "for _, t in ipairs(db_types()) do log_status('db type: ' .. t) end\n"
        "\n"
        "-- NPC usage:  workshop_show_token;  // prints the entry\n"
        "register_buildin('workshop_show_token', '', function(player)\n"
        "    local entry = db_get('ITEM_DB', 90001)\n"
        "    if not entry then\n"
        "        mes(player, 'sample_items.yml not loaded')\n"
        "    else\n"
        "        mes(player, entry.AegisName .. ' / ' .. entry.Name)\n"
        "        mes(player, 'Type=' .. entry.Type .. ' Weight=' .. entry.Weight)\n"
        "    end\n"
        "    close_dialog(player)\n"
        "end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- Status change demo (SC_*)\n"
        "-- ----------------------------------------------------------------\n"
        "-- sc_start(player, type, duration_ms [, v1..v4 [, flag]]) — type is\n"
        "-- either a number or an SC name ('FREEZE' / 'SC_FREEZE'). sc_id()\n"
        "-- caches the lookup; sc_active / sc_val / sc_end inspect or remove.\n"
        "--\n"
        "-- @freeze  -> 5s freeze, then reports it\n"
        "register_atcmd('freeze', 0, function(player)\n"
        "    if not player then return 0 end\n"
        "    sc_start(player, 'FREEZE', 5000)\n"
        "    if sc_active(player, 'FREEZE') then\n"
        "        message(player, 'You are frozen for 5s.')\n"
        "    end\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- @blessing  -> Blessing lv10 for 60s (val1 = skill level)\n"
        "register_atcmd('blessing', 0, function(player)\n"
        "    if not player then return 0 end\n"
        "    local sc = sc_id('BLESSING')          -- numeric id, cached\n"
        "    sc_start(player, sc, 60000, 10)\n"
        "    message(player, 'Blessing +' .. sc_val(player, sc, 1))\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- @cleanse  -> drop the normal removable statuses\n"
        "register_atcmd('cleanse', 0, function(player)\n"
        "    if player then sc_clear(player) end\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- A brand-new, plugin-defined SC. register_sc returns an id that\n"
        "-- sc_start / sc_end / sc_active / sc_val all accept just like an\n"
        "-- engine sc_type. calc_flag lists the stats it touches; the calc\n"
        "-- callback runs once per affected stat during status recalc and\n"
        "-- returns the new value.\n"
        "WORKSHOP_HYPER = register_sc('Workshop_Hyper', {'STR', 'AGI'}, function(ctx)\n"
        "    -- ctx.stat is a single PLUGIN_SCB_* bit; ctx.val1 = bonus amount.\n"
        "    return ctx.cur + (ctx.val1 or 0)\n"
        "end)\n"
        "\n"
        "-- @hyper [secs]  -> +25 STR/AGI for N seconds (default 30)\n"
        "register_atcmd('hyper', 0, function(player, args)\n"
        "    if not player then return 0 end\n"
        "    local secs = tonumber(args) or 30\n"
        "    sc_start(player, WORKSHOP_HYPER, secs * 1000, 25)\n"
        "    message(player, 'Hyper! +' .. sc_val(player, WORKSHOP_HYPER, 1)\n"
        "        .. ' STR/AGI for ' .. secs .. 's')\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- @hyper_off\n"
        "register_atcmd('hyper_off', 0, function(player)\n"
        "    if player and sc_active(player, WORKSHOP_HYPER) then\n"
        "        sc_end(player, WORKSHOP_HYPER)\n"
        "        message(player, 'Hyper ended.')\n"
        "    end\n"
        "    return 1\n"
        "end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- Reading rAthena's status.yml shape\n"
        "-- ----------------------------------------------------------------\n"
        "-- status.yml entries are keyed by the `Status:` name (no numeric\n"
        "-- Id), so a mod that wants to read it declares the key field:\n"
        "--   db:\n"
        "--     - path: db/my_status.yml\n"
        "--       key:  Status\n"
        "-- then queries with the name string. (db/sample_status.yml below is\n"
        "-- a tiny example; point at db/re/status.yml for the real thing.)\n"
        "if db_count('STATUS_DB') > 0 then\n"
        "    local s = db_get('STATUS_DB', 'Stone')\n"
        "    if s then log_status('STATUS_DB Stone -> ' .. tostring(s.DurationLookup)) end\n"
        "end\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- Client packet hooks\n"
        "-- ----------------------------------------------------------------\n"
        "-- register_packet(cmd, length, fn): handle a previously-unused\n"
        "-- packet id (cmd 0x064..0xCFF). `length` is the fixed size in\n"
        "-- bytes incl. the 2-byte cmd, or -1 for variable length.\n"
        "register_packet(0x0CFD, 2, function(ctx)\n"
        "    log_info('got custom packet 0x0CFD from fd ' .. ctx.fd)\n"
        "    if ctx.player then message(ctx.player, 'pong!') end\n"
        "end)\n"
        "\n"
        "-- on_packet(cmd, fn): intercept an *existing* client packet before\n"
        "-- the engine handles it. ctx = {fd, cmd, player?}; read the payload\n"
        "-- with packet_read_b/w/l/str(ctx.fd, offset). Return false or 'stop'\n"
        "-- to suppress the engine handler; nil/true lets it run.\n"
        "--\n"
        "-- Example (commented out — pick a packet id your client build uses):\n"
        "-- on_packet(0x0090, function(ctx)            -- 'talk to NPC'\n"
        "--     local npc_id = packet_read_l(ctx.fd, 2)\n"
        "--     log_info('NPC click: npc_id=' .. npc_id)\n"
        "--     -- return false   -- would block the interaction\n"
        "-- end)\n"
        "\n"
        "-- ----------------------------------------------------------------\n"
        "-- Battle config (conf/battle/*.conf) at runtime\n"
        "-- ----------------------------------------------------------------\n"
        "log_status('base_exp_rate = ' .. battle_get('base_exp_rate'))\n"
        "\n"
        "-- @setrate [pct]  -> set base & job EXP rate (no arg = report)\n"
        "register_atcmd('setrate', 99, function(player, args)\n"
        "    local r = tonumber(args)\n"
        "    if not r then\n"
        "        if player then\n"
        "            message(player, 'base_exp_rate = ' .. battle_get('base_exp_rate'))\n"
        "        end\n"
        "        return 1\n"
        "    end\n"
        "    battle_set('base_exp_rate', r)\n"
        "    battle_set('job_exp_rate', r)\n"
        "    if player then message(player, 'EXP rate -> ' .. r .. '%') end\n"
        "    return 1\n"
        "end)\n";
    write_text(scripts + "/hello.lua", hello);

    wlog_status("scaffolded example mod at %s", ex.c_str());
    return true;
}

} // namespace workshop
