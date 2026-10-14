#include "merge.hpp"

#include <format>
#include <optional>
#include <unordered_set>

namespace discovery {

namespace {

constexpr const char* kSections[] = {"pca", "relays", "pwm", "sensors", "covers"};

bool is_section(std::string_view k) {
    for (const char* s : kSections)
        if (k == s) return true;
    return false;
}

// Entry identity: the "id" value (number or string) as text.
std::optional<std::string> entry_key(const json::value& v) {
    if (!v.is_object()) return std::nullopt;
    const auto* id = v.as_object().if_contains("id");
    if (!id) return std::nullopt;
    if (id->is_int64())  return std::to_string(id->as_int64());
    if (id->is_uint64()) return std::to_string(id->as_uint64());
    if (id->is_string()) return std::string(id->as_string());
    return std::nullopt;
}

std::string name_of(const json::object& o) {
    const auto* n = o.if_contains("name");
    return (n && n->is_string()) ? std::string(n->as_string()) : std::string{};
}

// Existing entry is the base; `name` and "_" diagnostics come from the device (the tool cannot
// tell a user-edited name from a device name); other keys only fill gaps.
json::object merge_entry(const json::object& existing, const json::object& fresh) {
    json::object out = existing;
    for (const auto& [k, v] : fresh) {
        const std::string_view key = k;
        if (key == "name" || key.starts_with('_')) {
            out[key] = v;
        } else if (const auto* cur = out.if_contains(key)) {
            // Nested objects (cover up/down): recurse so their "_" diagnostics refresh too.
            if (cur->is_object() && v.is_object())
                out[key] = merge_entry(cur->as_object(), v.as_object());
        } else {
            out[key] = v;
        }
    }
    return out;
}

json::array merge_section(const json::array& existing, const json::array& fresh,
                          const std::string& server_id, const char* section,
                          MergeReport& report) {
    json::array out;
    std::unordered_set<std::string> seen;

    for (const auto& fv : fresh) {
        const auto key = entry_key(fv);
        if (!key) { out.push_back(fv); continue; }
        seen.insert(*key);

        const json::object* match = nullptr;
        for (const auto& ev : existing) {
            const auto ek = entry_key(ev);
            if (ek && *ek == *key) { match = &ev.as_object(); break; }
        }
        if (match) {
            out.push_back(merge_entry(*match, fv.as_object()));
        } else {
            report.push_back(std::format("server {}: new {} '{}' ({}) added",
                                         server_id, section, *key, name_of(fv.as_object())));
            out.push_back(fv);
        }
    }

    for (const auto& ev : existing) {
        const auto key = entry_key(ev);
        if (key && seen.contains(*key)) continue;
        if (key)
            report.push_back(std::format(
                "server {}: {} '{}' ({}) no longer reported by the device - kept, review/remove manually",
                server_id, section, *key, name_of(ev.as_object())));
        out.push_back(ev);
    }
    return out;
}

std::string server_id_of(const json::object& o) {
    const auto* id = o.if_contains("id");
    return (id && id->is_string()) ? std::string(id->as_string()) : std::string{};
}

} // namespace

json::object merge_server(const json::object* existing, const json::object& fresh,
                          MergeReport& report) {
    if (!existing) return fresh;

    const std::string sid = server_id_of(fresh);
    json::object out = *existing;

    // The server answered this run: a stub's `_error` from an earlier failed run is stale.
    out.erase("_error");

    // Server-level keys: existing wins, gaps filled from the fresh entry.
    for (const auto& [k, v] : fresh)
        if (!is_section(k) && !out.contains(k))
            out[k] = v;

    for (const char* section : kSections) {
        const auto* ex = existing->if_contains(section);
        const auto* fr = fresh.if_contains(section);
        if (ex && !ex->is_array()) continue;  // malformed user edit: leave untouched

        static const json::array kEmpty;
        const json::array& ea = ex ? ex->as_array() : kEmpty;
        const json::array& fa = (fr && fr->is_array()) ? fr->as_array() : kEmpty;
        if (ea.empty() && fa.empty()) continue;
        out[section] = merge_section(ea, fa, sid, section, report);
    }
    return out;
}

json::object failed_server(const json::object* existing, const std::string& id,
                           const std::string& host, const std::string& error,
                           MergeReport& report) {
    if (existing) {
        report.push_back(std::format("server {}: query failed ({}) - existing config kept unchanged",
                                     id, error));
        return *existing;
    }
    return json::object{{"id", id}, {"host", host}, {"_error", error}};
}

const json::object* find_server(const json::object& root, std::string_view id) {
    const auto* arr = root.if_contains("servers");
    if (!arr || !arr->is_array()) return nullptr;
    for (const auto& s : arr->as_array())
        if (s.is_object() && server_id_of(s.as_object()) == id)
            return &s.as_object();
    return nullptr;
}

} // namespace discovery
