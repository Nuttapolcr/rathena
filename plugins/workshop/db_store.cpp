#include "db_store.hpp"
#include "workshop.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

namespace workshop {

DbStore& DbStore::instance() {
    static DbStore inst;
    return inst;
}

OverrideMode parse_override_mode(const std::string& s) {
    if (s.empty() || s == "replace") return OverrideMode::Replace;
    if (s == "skip")    return OverrideMode::Skip;
    if (s == "error")   return OverrideMode::Error;
    if (s == "merge")   return OverrideMode::Merge;
    wlog_warning("db: unknown override mode '%s' — falling back to 'replace'",
                 s.c_str());
    return OverrideMode::Replace;
}

// Recursive deep-merge of `src` into `dst`. Maps are walked key by key;
// any non-map field on either side is replaced by `src`. Sequences are
// replaced wholesale (rAthena DB conventions don't have a stable per-
// element merge — list-typed fields like `Trade` are usually overridden
// as a unit anyway).
static void deep_merge(YAML::Node dst, const YAML::Node& src) {
    if (!src.IsMap() || !dst.IsMap()) {
        dst = src;
        return;
    }
    for (auto kv : src) {
        auto k = kv.first.as<std::string>();
        if (dst[k] && dst[k].IsMap() && kv.second.IsMap()) {
            YAML::Node merged = YAML::Clone(dst[k]);
            deep_merge(merged, kv.second);
            dst[k] = merged;
        } else {
            dst[k] = YAML::Clone(kv.second);
        }
    }
}

bool DbStore::load_file(const std::string& path,
                        const std::string& mod_name,
                        const std::string& key_field_in,
                        OverrideMode mode) {
    std::string key_field = key_field_in.empty() ? "Id" : key_field_in;

    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        wlog_error("db: %s: parse failed: %s", path.c_str(), e.what());
        return false;
    }

    YAML::Node header = root["Header"];
    YAML::Node body   = root["Body"];

    if (!header || !header["Type"]) {
        wlog_error("db: %s: missing Header.Type", path.c_str());
        return false;
    }
    if (!body || !body.IsSequence()) {
        wlog_error("db: %s: missing Body sequence", path.c_str());
        return false;
    }

    std::string type = header["Type"].as<std::string>();
    auto& bucket = store_[type];

    int loaded = 0, replaced = 0, skipped = 0, merged = 0;
    for (auto entry : body) {
        if (!entry[key_field]) {
            wlog_warning("db: %s: entry missing key '%s'",
                         path.c_str(), key_field.c_str());
            continue;
        }

        int64_t id = 0;
        try {
            id = entry[key_field].as<int64_t>();
        } catch (const std::exception&) {
            wlog_warning("db: %s: non-integer %s in entry — skipping",
                         path.c_str(), key_field.c_str());
            continue;
        }

        auto it = bucket.find(id);
        if (it == bucket.end()) {
            bucket[id] = DbEntry{mod_name, YAML::Clone(entry)};
            ++loaded;
            continue;
        }

        switch (mode) {
        case OverrideMode::Replace:
            bucket[id] = DbEntry{mod_name, YAML::Clone(entry)};
            ++replaced;
            break;
        case OverrideMode::Skip:
            ++skipped;
            break;
        case OverrideMode::Error:
            wlog_error("db: %s: %s id=%lld already loaded by mod '%s' "
                       "(override: error) — skipping",
                       path.c_str(), type.c_str(), (long long)id,
                       it->second.mod_name.c_str());
            ++skipped;
            break;
        case OverrideMode::Merge: {
            YAML::Node out = YAML::Clone(it->second.body);
            deep_merge(out, entry);
            bucket[id] = DbEntry{mod_name, out};
            ++merged;
            break;
        }
        }
    }

    wlog_status("db: %s from %s: +%d replaced=%d skipped=%d merged=%d "
                "(mod=%s)",
                type.c_str(), path.c_str(),
                loaded, replaced, skipped, merged, mod_name.c_str());
    return true;
}

bool DbStore::has(const std::string& type, int64_t id) const {
    auto it = store_.find(type);
    if (it == store_.end()) return false;
    return it->second.find(id) != it->second.end();
}

YAML::Node DbStore::get(const std::string& type, int64_t id) const {
    auto it = store_.find(type);
    if (it == store_.end()) return YAML::Node();
    auto eit = it->second.find(id);
    if (eit == it->second.end()) return YAML::Node();
    return eit->second.body;
}

size_t DbStore::count(const std::string& type) const {
    auto it = store_.find(type);
    return it == store_.end() ? 0 : it->second.size();
}

void DbStore::each(const std::string& type,
                   const std::function<void(int64_t, const YAML::Node&)>& fn) const {
    auto it = store_.find(type);
    if (it == store_.end()) return;
    for (auto& kv : it->second) {
        fn(kv.first, kv.second.body);
    }
}

std::vector<std::string> DbStore::types() const {
    std::vector<std::string> out;
    out.reserve(store_.size());
    for (auto& kv : store_) out.push_back(kv.first);
    return out;
}

void DbStore::clear() {
    store_.clear();
}

} // namespace workshop
