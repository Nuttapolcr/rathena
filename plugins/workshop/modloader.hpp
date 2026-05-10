#ifndef WORKSHOP_MODLOADER_HPP
#define WORKSHOP_MODLOADER_HPP

#include <string>
#include <vector>

namespace workshop {

// One DB-source declaration from a mod's modinfo.yml. Each file is a
// rAthena-style YAML (`Header.Type` + `Body` sequence); the workshop
// indexes its entries into a shared store.
struct DbFile {
    std::string path;            // relative path inside the mod folder
    std::string key_field;       // "" → defaults to "Id"
    std::string override_mode;   // "" → defaults to "replace"
};

struct ModInfo {
    std::string dir;            // absolute path to the mod folder
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::vector<std::string> scripts;       // relative paths inside the mod dir
    std::vector<std::string> dependencies;  // names of mods that must load first
    std::vector<DbFile>      db_files;      // YAML DB files to register before scripts
    int load_order = 100;                   // lower runs first; default 100
    std::string on_init;                    // optional Lua function name to call after load
    bool enabled = true;                    // false = mod is read but not loaded
};

class ModLoader {
public:
    static ModLoader& instance();

    // Discover every <mods_dir>/<name>/modinfo.yml, parse them, and resolve
    // load order based on `load_order` + topological sort over `dependencies`.
    // Returns false if a cycle is detected or a dependency is missing.
    bool discover(const std::string& mods_dir);

    // Load every discovered mod into the shared Lua VM (in resolved order),
    // running the mod's script files and calling its on_init hook (if any).
    bool load_all();

    const std::vector<ModInfo>& mods() const { return ordered_; }

private:
    ModLoader() = default;

    std::vector<ModInfo> ordered_;
};

// Create the mods/ directory if missing, and write an example mod into it
// the first time the plugin runs (when the directory was just created).
// Returns true if anything was scaffolded.
bool scaffold_mods_dir(const std::string& mods_dir);

} // namespace workshop

#endif
