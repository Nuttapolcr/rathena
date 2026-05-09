#include "modloader.hpp"
#include "lua_bridge.hpp"
#include "workshop.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace workshop {

ModLoader& ModLoader::instance() {
    static ModLoader inst;
    return inst;
}

// ---- helpers ----

static bool dir_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::vector<std::string> list_subdirs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (auto* ent = readdir(d)) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        if (dir_exists(full)) out.push_back(ent->d_name);
    }
    closedir(d);
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
    mi.scripts      = read_string_seq(body["scripts"]);
    mi.dependencies = read_string_seq(body["dependencies"]);
    mi.load_order   = body["load_order"] ? body["load_order"].as<int>() : 100;
    mi.on_init      = body["on_init"]    ? body["on_init"].as<std::string>() : "";
    mi.enabled      = body["enabled"]    ? body["enabled"].as<bool>()      : true;

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

    if (mkdir(ex.c_str(), 0755) != 0) return false;
    std::string scripts = ex + "/scripts";
    mkdir(scripts.c_str(), 0755);

    std::string modinfo =
        "# modinfo.yml — describes a workshop mod.\n"
        "#\n"
        "# Required:  name, scripts\n"
        "# Optional:  enabled (default true), version, author, description,\n"
        "#            dependencies, load_order, on_init\n"
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
        "on_init: example_on_init\n"
        "# dependencies:\n"
        "#   - other_mod_name\n";
    write_text(ex + "/modinfo.yml", modinfo);

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
        "end)\n";
    write_text(scripts + "/hello.lua", hello);

    wlog_status("scaffolded example mod at %s", ex.c_str());
    return true;
}

} // namespace workshop
