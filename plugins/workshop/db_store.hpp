#ifndef WORKSHOP_DB_STORE_HPP
#define WORKSHOP_DB_STORE_HPP

#include <yaml-cpp/yaml.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace workshop {

// What to do when an entry's primary key collides with one already in the
// store. Tracked per-file so different mods/files can mix policies.
enum class OverrideMode {
    Replace,    // newer wins (default — same as rAthena's import/ folder)
    Skip,       // first wins; later attempts are ignored
    Error,      // log + skip; useful while debugging mod conflicts
    Merge,      // deep-merge YAML maps key by key (later mod tweaks fields)
};

struct DbEntry {
    std::string mod_name;   // who supplied the current value (for diagnostics)
    YAML::Node  body;       // a Body[i] node from the YAML source
};

// Singleton holding every DB entry the workshop has parsed across all mods.
// Keyed by Header.Type (e.g. "ITEM_DB", "MOB_DB") then by the entry's primary
// key (numeric, default field name "Id").
class DbStore {
public:
    static DbStore& instance();

    // Load one rAthena-style YAML file. Reads `Header.Type`, iterates `Body`
    // and indexes each entry by `key_field` (numeric). Applies `mode` to
    // resolve collisions.
    bool load_file(const std::string& path,
                   const std::string& mod_name,
                   const std::string& key_field,   // "" → "Id"
                   OverrideMode       mode);

    // ---- queries ----
    bool       has  (const std::string& type, int64_t id) const;
    YAML::Node get  (const std::string& type, int64_t id) const;
    size_t     count(const std::string& type) const;

    void each(const std::string& type,
              const std::function<void(int64_t, const YAML::Node&)>& fn) const;

    std::vector<std::string> types() const;

    // Drop everything — called during workshop_reload and plugin_final.
    void clear();

private:
    DbStore() = default;
    DbStore(const DbStore&) = delete;

    // type → id → entry
    std::map<std::string, std::map<int64_t, DbEntry>> store_;
};

// Parse the override-mode string from modinfo.yml; unknown values map to
// OverrideMode::Replace and a warning is logged at the call site.
OverrideMode parse_override_mode(const std::string& s);

} // namespace workshop

#endif
