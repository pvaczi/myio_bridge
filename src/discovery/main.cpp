#include "merge.hpp"
#include <log.hpp>
#include <dsens_parser.hpp>
#include <http_client.hpp>
#include <types.hpp>

#include <boost/json.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace json = boost::json;

// ── Input configuration ───────────────────────────────────────────────────────

struct InputServer {
    std::string   id;
    std::string   host;
    std::uint16_t port             = 80;
    std::string   user             = "admin";
    std::string   password         = "admin";
    std::string   pca_default_type      = "switch";  // default HA type for PCA outputs
    int           poll_interval_sec     = 5;
    double        sensor_log_threshold  = 0.1;
    bool          log_startup_sync      = false;
    bool          log_debug             = false;
    int           coalesce_window_ms    = 5;
};

struct InputConfig {
    std::vector<InputServer> servers;
    json::object             mqtt = {               // passed through verbatim to bridge.json
        {"host",         "192.168.1.100"},
        {"port",         1883},
        {"client_id",    "myio_bridge"},
        {"user",         ""},
        {"password",     ""},
        {"topic_prefix", "myio"}
    };
};

static InputConfig read_input(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error(std::format("Cannot open: {}", path));

    const std::string content{std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>()};
    const json::value root = json::parse(content);
    const auto& obj = root.as_object();

    InputConfig cfg;

    if (const auto* mqtt = obj.if_contains("mqtt"))
        if (const auto* o = mqtt->if_object())
            cfg.mqtt = *o;

    if (const auto* arr = obj.if_contains("servers")) {
        for (const auto& node : arr->as_array()) {
            const auto& s = node.as_object();
            InputServer is;
            if (const auto* v = s.if_contains("id"))               is.id               = std::string(v->as_string());
            if (const auto* v = s.if_contains("host"))             is.host             = std::string(v->as_string());
            if (const auto* v = s.if_contains("port"))             is.port             = static_cast<std::uint16_t>(v->as_int64());
            if (const auto* v = s.if_contains("user"))             is.user             = std::string(v->as_string());
            if (const auto* v = s.if_contains("password"))         is.password         = std::string(v->as_string());
            if (const auto* v = s.if_contains("pca_default_type"))  is.pca_default_type  = std::string(v->as_string());
            if (const auto* v = s.if_contains("poll_interval_sec"))    is.poll_interval_sec    = static_cast<int>(v->as_int64());
            if (const auto* v = s.if_contains("sensor_log_threshold")) {
                if (v->is_double())  is.sensor_log_threshold = v->as_double();
                else if (v->is_int64()) is.sensor_log_threshold = static_cast<double>(v->as_int64());
            }
            if (const auto* v = s.if_contains("log_startup_sync"))    if (v->is_bool()) is.log_startup_sync = v->as_bool();
            if (const auto* v = s.if_contains("log_debug"))           if (v->is_bool()) is.log_debug        = v->as_bool();
            if (const auto* v = s.if_contains("coalesce_window_ms"))   is.coalesce_window_ms   = static_cast<int>(v->as_int64());
            if (is.id.empty() || is.host.empty())
                throw std::runtime_error("Server entry requires 'id' and 'host'");
            cfg.servers.push_back(std::move(is));
        }
    }
    return cfg;
}

// ── id → name lookup map ──────────────────────────────────────────────────────

static std::unordered_map<int, std::string>
build_name_map(const myio::ServerState& state) {
    std::unordered_map<int, std::string> m;
    for (const auto& p : state.pca_outputs)   m[p.id] = p.name;
    for (const auto& r : state.relay_outputs)  m[r.id] = r.name;
    for (const auto& w : state.pwm_outputs)    m[w.id] = w.name;
    return m;
}

// ╔══════════════════════════════════════════════════════════════════════════════╗
// ║  FIRMWARE BUG WORKAROUND                                                    ║
// ║  Bug:       d_sens_out.json returns empty descriptions for PCA id > 2100.   ║
// ║  Workaround: fetch /p_desc.xml which returns all PCA names correctly.       ║
// ║  Remove:    delete fetch_pca_desc() and its call site in make_server_entry  ║
// ║             once the firmware bug is fixed.                                  ║
// ╚══════════════════════════════════════════════════════════════════════════════╝
//
// /p_desc.xml format: <N>name</N> where N is 1-based index (real_id = N + 2000).
// The file is served by the device's HTTP server and is also used by the web UI.

static std::unordered_map<int, std::string>
fetch_pca_desc(const myio::HttpConfig& hcfg) {
    std::unordered_map<int, std::string> names;
    std::string body;
    try {
        body = myio::http_get_raw(hcfg, "/p_desc.xml");
    } catch (const std::exception& ex) {
        myio::log(stderr, "[discovery] /p_desc.xml unavailable ({}): {}", hcfg.server_id, myio::short_what(ex));
        return names;
    }

    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto open = body.find('<', pos);
        if (open == std::string::npos) break;
        const auto gt = body.find('>', open);
        if (gt == std::string::npos) break;

        const std::string tag = body.substr(open + 1, gt - open - 1);
        pos = gt + 1;

        if (tag.empty() || tag[0] == '/') continue;
        bool all_digits = std::ranges::all_of(tag,
            [](unsigned char c) { return std::isdigit(c); });
        if (!all_digits) continue;

        const int idx = std::stoi(tag);
        const std::string end_tag = "</" + tag + ">";
        const auto end = body.find(end_tag, pos);
        if (end == std::string::npos) continue;

        std::string name = body.substr(pos, end - pos);
        pos = end + end_tag.size();

        if (!name.empty())
            names[idx + 2000] = std::move(name);
    }
    return names;
}
// ── END FIRMWARE BUG WORKAROUND ───────────────────────────────────────────────

// ── Existing bridge.json ──────────────────────────────────────────────────────
// Parsed as-is; discovery::merge_server() keeps every user-set key except `name`,
// which is refreshed from the device (see merge.hpp).

static json::object read_existing(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    try {
        const std::string content{std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>()};
        const json::value root = json::parse(content);
        if (root.is_object()) return root.as_object();
    } catch (...) {}
    return {};
}

// ── Cover candidate detection ─────────────────────────────────────────────────
// Returns {up_id, down_id} pairs derived from protection pairs.
// Only same-type pairs qualify (both relay or both PCA); mixed-type pairs
// are protection/inhibit connections, not motor covers.
// Heuristic: if either name contains "down/le/lent", swap the order.

struct CoverCandidate {
    int         up_id;
    int         down_id;
    std::string output_type;  // "pca" or "relay"
    int         protection_index;
};

static std::vector<CoverCandidate>
detect_covers(const myio::ServerState& state,
              const std::unordered_map<int, std::string>& name_map) {
    std::unordered_set<int> relay_ids, pca_ids;
    for (const auto& r : state.relay_outputs) relay_ids.insert(r.id);
    for (const auto& p : state.pca_outputs)   pca_ids.insert(p.id);

    std::vector<CoverCandidate> covers;
    for (const auto& pair : state.protection_pairs) {
        // Skip mixed-type pairs (e.g. relay inhibiting a PCA output)
        const bool both_relay = relay_ids.contains(pair.element0)
                             && relay_ids.contains(pair.element1);
        const bool both_pca   = pca_ids.contains(pair.element0)
                             && pca_ids.contains(pair.element1);
        if (!both_relay && !both_pca) continue;

        const std::string output_type = both_relay ? "relay" : "pca";

        auto get_name = [&](int id) -> std::string {
            auto it = name_map.find(id);
            return (it != name_map.end()) ? it->second : "";
        };
        const std::string name0 = get_name(pair.element0);

        auto name_lc = [](const std::string& s) {
            std::string sl = s;
            std::ranges::transform(sl, sl.begin(), ::tolower);
            return sl;
        };
        const std::string lc0 = name_lc(name0);

        int up_id   = pair.element0;
        int down_id = pair.element1;
        // If name of element0 suggests "down", swap
        if (lc0.contains("down") || lc0.contains(" le") || lc0.contains("lent"))
            std::swap(up_id, down_id);

        covers.push_back({up_id, down_id, output_type, pair.index});
    }
    return covers;
}

// ── Pretty-print JSON ─────────────────────────────────────────────────────────

static void write_json(std::ostream& out, const json::value& jv, int indent = 0) {
    const std::string pad (static_cast<std::size_t>(indent * 2),       ' ');
    const std::string pad2(static_cast<std::size_t>((indent + 1) * 2), ' ');
    switch (jv.kind()) {
    case json::kind::object: {
        out << "{\n";
        const auto& o = jv.get_object();
        std::size_t n = 0;
        for (const auto& [k, v] : o) {
            out << pad2 << json::serialize(k) << ": ";
            write_json(out, v, indent + 1);
            if (++n < o.size()) out << ',';
            out << '\n';
        }
        out << pad << '}';
        break;
    }
    case json::kind::array: {
        const auto& a = jv.get_array();
        if (a.empty()) { out << "[]"; break; }
        const bool all_scalar = std::ranges::all_of(a, [](const json::value& v) {
            return v.is_number() || v.is_bool() || v.is_null();
        });
        if (all_scalar && a.size() <= 16) {
            out << '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out << ", ";
                out << json::serialize(a[i]);
            }
            out << ']';
        } else {
            out << "[\n";
            for (std::size_t i = 0; i < a.size(); ++i) {
                out << pad2;
                write_json(out, a[i], indent + 1);
                if (i + 1 < a.size()) out << ',';
                out << '\n';
            }
            out << pad << ']';
        }
        break;
    }
    default:
        out << json::serialize(jv);
        break;
    }
}

// ── JSON output builders ──────────────────────────────────────────────────────
// Diagnostic fields prefixed with _ are kept only when they help configuration:
//   _pwm            – clarifies why type was set to "light"
//   _sensor_id      – sensor activator (1..254 = sensor, 255 = SUN); type is sensor_output only for 1..254
//   _protection_index – links cover output to its protection pair
// Removed: _state, _turn_off, _turn_on, _sensor_on, _sensor_off,
//          _temperature_c, _humidity_pct, _energy_kwh, _power_kw,
//          _voltage_v, _current_a  (all are stale snapshots)

static json::object make_pca_entry(const myio::PcaOutput& p,
                                    const std::string& pca_default_type,
                                    const std::string& name,
                                    const std::string& type_override = "") {
    json::object o;
    o["id"]      = p.id;
    o["name"]    = name;
    o["type"]    = type_override.empty()
                     ? (p.sensor_controlled ? "sensor_output" : pca_default_type)
                     : type_override;
    o["enabled"] = false;
    // Keep only diagnostics that aid configuration decisions
    if (p.is_pwm)            o["_pwm"]       = true;
    if (p.sensor_id != 0)         o["_sensor_id"] = p.sensor_id;
    return o;
}

static json::object make_relay_entry(const myio::RelayOutput& r,
                                      const std::string& type_override = "") {
    json::object o;
    o["id"]      = r.id;
    o["name"]    = r.name;
    o["type"]    = type_override.empty()
                     ? (r.sensor_controlled ? "sensor_output" : "switch")
                     : type_override;
    o["enabled"] = false;
    if (r.sensor_id != 0)         o["_sensor_id"] = r.sensor_id;
    return o;
}

static json::object make_pwm_entry(const myio::PwmOutput& p) {
    json::object o;
    o["id"]      = p.id;
    o["name"]    = p.name;
    o["type"]    = p.sensor_controlled ? "sensor_output" : "light";
    o["enabled"] = false;
    if (p.sensor_id != 0)         o["_sensor_id"] = p.sensor_id;
    return o;
}

static json::object make_sensor_entry(const myio::Sensor& s) {
    json::object o;
    o["id"]      = s.id;
    o["name"]    = s.name;
    o["enabled"] = false;
    // No diagnostic fields – sensor type is obvious from the id range
    return o;
}

static json::array make_cover_array(
    const std::vector<CoverCandidate>& candidates,
    const std::unordered_map<int, std::string>& name_map,
    const std::unordered_map<int, int>& timer_map)   // id → timer_sec (0 = none)
{
    json::array covers;
    for (const auto& c : candidates) {
        auto get_name = [&](int id) {
            auto it = name_map.find(id);
            return (it != name_map.end() && !it->second.empty())
                     ? it->second : std::format("id_{}", id);
        };
        auto get_timer = [&](int id) {
            auto it = timer_map.find(id);
            return (it != timer_map.end()) ? it->second : 0;
        };
        json::object cover;
        cover["id"]      = std::format("cover_{}", c.protection_index);
        cover["name"]    = get_name(c.up_id);  // name suggestion from UP output
        cover["enabled"] = false;
        // Use the larger of the two endpoint timers (guards against misconfiguration)
        const int timer = std::max(get_timer(c.up_id), get_timer(c.down_id));
        if (timer > 0) cover["motor_timeout_sec"] = timer;
        cover["up"]   = json::object{
            {"type", c.output_type}, {"id", c.up_id},
            {"_name", get_name(c.up_id)}
        };
        cover["down"] = json::object{
            {"type", c.output_type}, {"id", c.down_id},
            {"_name", get_name(c.down_id)}
        };
        cover["_protection_index"] = c.protection_index;
        covers.push_back(std::move(cover));
    }
    return covers;
}

static json::object make_server_entry(const InputServer& is,
                                       const myio::ServerState& state,
                                       const myio::HttpConfig& hcfg,
                                       const std::string& pca_default_type) {
    auto name_map = build_name_map(state);

    // ── FIRMWARE BUG WORKAROUND: supplement empty PCA names (id > 2100) ──────────
    // Remove fetch_pca_desc() call and this block when firmware is fixed.
    const auto pca_desc = fetch_pca_desc(hcfg);
    for (auto& [id, name] : name_map)
        if (name.empty() && pca_desc.count(id))
            name = pca_desc.at(id);
    // ── END FIRMWARE BUG WORKAROUND ───────────────────────────────────────────────

    // id → motor timer in seconds (0-based PCA index → real id 2001+)
    std::unordered_map<int, int> pca_timer_map;
    for (const auto& p : state.pca_outputs)
        if (p.timer_sec > 0) pca_timer_map[p.id] = p.timer_sec;

    const auto cover_cands = detect_covers(state, name_map);

    // Build set of output ids that are part of a cover
    std::unordered_set<int> cover_ids;
    for (const auto& c : cover_cands) {
        cover_ids.insert(c.up_id);
        cover_ids.insert(c.down_id);
    }

    json::object srv;
    srv["id"]                = is.id;
    srv["host"]              = is.host;
    srv["port"]              = is.port;
    srv["user"]              = is.user;
    srv["password"]          = is.password;  // TODO: encryption
    srv["poll_interval_sec"]    = is.poll_interval_sec;
    srv["sensor_log_threshold"] = is.sensor_log_threshold;
    srv["log_startup_sync"]     = is.log_startup_sync;
    srv["log_debug"]            = is.log_debug;
    srv["coalesce_window_ms"]   = is.coalesce_window_ms;

    // PCA outputs – named only; cover endpoints get "cover_up" / "cover_down" type
    json::array pca_arr;
    for (const auto& p : state.pca_outputs) {
        const std::string name = name_map.count(p.id) ? name_map.at(p.id) : p.name;
        if (name.empty()) continue;
        std::string type_override;
        for (const auto& c : cover_cands) {
            if (p.id == c.up_id)   { type_override = "cover_up";   break; }
            if (p.id == c.down_id) { type_override = "cover_down"; break; }
        }
        pca_arr.push_back(make_pca_entry(p, pca_default_type, name, type_override));
    }
    if (!pca_arr.empty()) srv["pca"] = std::move(pca_arr);

    // Relay outputs – named only; same cover tagging
    json::array relay_arr;
    for (const auto& r : state.relay_outputs) {
        if (r.name.empty()) continue;
        std::string type_override;
        for (const auto& c : cover_cands) {
            if (r.id == c.up_id)   { type_override = "cover_up";   break; }
            if (r.id == c.down_id) { type_override = "cover_down"; break; }
        }
        relay_arr.push_back(make_relay_entry(r, type_override));
    }
    if (!relay_arr.empty()) srv["relays"] = std::move(relay_arr);

    // PWM outputs – named only
    json::array pwm_arr;
    for (const auto& p : state.pwm_outputs) {
        if (p.name.empty()) continue;
        pwm_arr.push_back(make_pwm_entry(p));
    }
    if (!pwm_arr.empty()) srv["pwm"] = std::move(pwm_arr);

    // Sensors – named only, with at least one actual measurement value.
    // Entries with all-nullopt fields are phantom/unused hardware slots.
    json::array sensor_arr;
    for (const auto& s : state.sensors) {
        if (s.name.empty()) continue;
        if (!s.temperature && !s.humidity && !s.energy &&
            !s.power && !s.voltage && !s.current) continue;
        sensor_arr.push_back(make_sensor_entry(s));
    }
    if (!sensor_arr.empty()) srv["sensors"] = std::move(sensor_arr);

    // Covers – omit section entirely if no candidates detected
    json::array cover_arr = make_cover_array(cover_cands, name_map, pca_timer_map);
    if (!cover_arr.empty()) srv["covers"] = std::move(cover_arr);

    return srv;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const std::string input_path  = (argc > 1) ? argv[1] : "discovery_input.json";
    const std::string output_path = (argc > 2) ? argv[2] : "bridge.json";

    myio::log("[discovery] Input:  {}", input_path);
    myio::log("[discovery] Output: {}", output_path);

    InputConfig input;
    try {
        input = read_input(input_path);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[discovery] Input error: {}", ex.what());
        return EXIT_FAILURE;
    }

    if (input.servers.empty()) {
        myio::log(stderr, "[discovery] No servers in input file.");
        return EXIT_FAILURE;
    }

    // Existing bridge.json: user-set keys are merged back in, except `name`, which
    // is refreshed from the device (see merge.hpp)
    const json::object existing = read_existing(output_path);
    if (!existing.empty())
        myio::log("[discovery] Merging into existing {} (user-set keys preserved, names refreshed from the device)",
                  output_path);

    json::object root = existing;   // unknown root-level keys round-trip
    root["mqtt"] = input.mqtt;

    json::array servers_arr;
    discovery::MergeReport report;
    bool any_failed = false;

    for (const auto& is : input.servers) {
        myio::log("[discovery] Querying: {} ({}:{}) ...", is.id, is.host, is.port);

        myio::HttpConfig hcfg;
        hcfg.host      = is.host;
        hcfg.port      = is.port;
        hcfg.user      = is.user;
        hcfg.password  = is.password;
        hcfg.server_id = is.id;

        const json::object* old_srv = discovery::find_server(existing, is.id);

        try {
            const std::string body  = myio::http_get(hcfg, "/d_sens_out.json");
            const auto        state = myio::DSensOutParser::parse(body, is.id);

            myio::log("[discovery]   PCA outputs:      {}", state.pca_outputs.size());
            myio::log("[discovery]   Relay outputs:    {}", state.relay_outputs.size());
            myio::log("[discovery]   PWM outputs:      {}", state.pwm_outputs.size());
            myio::log("[discovery]   Sensors:          {}", state.sensors.size());
            myio::log("[discovery]   Protection pairs: {} → cover candidates",
                      state.protection_pairs.size());

            servers_arr.push_back(discovery::merge_server(
                old_srv, make_server_entry(is, state, hcfg, is.pca_default_type), report));

        } catch (const std::exception& ex) {
            myio::log(stderr, "[discovery] ERROR ({}): {}", is.id, myio::short_what(ex));
            any_failed = true;
            servers_arr.push_back(discovery::failed_server(
                old_srv, is.id, is.host, myio::short_what(ex), report));
        }
    }

    // Servers in the existing file that are not in the input: keep, never drop silently
    if (const auto* old_servers = existing.if_contains("servers"); old_servers && old_servers->is_array()) {
        for (const auto& os : old_servers->as_array()) {
            if (!os.is_object()) continue;
            const auto* oid = os.as_object().if_contains("id");
            const bool listed = oid && oid->is_string() &&
                std::ranges::any_of(input.servers, [&](const InputServer& s) {
                    return s.id == oid->as_string();
                });
            if (listed) continue;
            report.push_back(std::format("server {}: not in the input file - kept unchanged",
                                         (oid && oid->is_string()) ? std::string(oid->as_string()) : "?"));
            servers_arr.push_back(os);
        }
    }

    root["servers"] = std::move(servers_arr);
    for (const auto& line : report)
        myio::log("[discovery] NOTE: {}", line);

    std::ofstream out(output_path);
    if (!out) {
        myio::log(stderr, "[discovery] Cannot write: {}", output_path);
        return EXIT_FAILURE;
    }
    write_json(out, root);
    out << '\n';

    myio::log("[discovery] Done → {}", output_path);
    if (any_failed)
        myio::log(stderr, "[discovery] Warning: one or more servers were unreachable.");

    return any_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
