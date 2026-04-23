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
    std::uint16_t port     = 80;
    std::string   user     = "admin";
    std::string   password = "admin";
};

static std::vector<InputServer> read_input(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error(std::format("Cannot open: {}", path));

    const std::string content{std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>()};
    const json::value root = json::parse(content);
    const auto& obj = root.as_object();

    std::vector<InputServer> servers;
    if (const auto* arr = obj.if_contains("servers")) {
        for (const auto& node : arr->as_array()) {
            const auto& s = node.as_object();
            InputServer is;
            if (const auto* v = s.if_contains("id"))       is.id       = std::string(v->as_string());
            if (const auto* v = s.if_contains("host"))     is.host     = std::string(v->as_string());
            if (const auto* v = s.if_contains("port"))     is.port     = static_cast<std::uint16_t>(v->as_int64());
            if (const auto* v = s.if_contains("user"))     is.user     = std::string(v->as_string());
            if (const auto* v = s.if_contains("password")) is.password = std::string(v->as_string());
            if (is.id.empty() || is.host.empty())
                throw std::runtime_error("Server entry requires 'id' and 'host'");
            servers.push_back(std::move(is));
        }
    }
    return servers;
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

// ── Cover candidate detection ─────────────────────────────────────────────────
// Returns {up_id, down_id} pairs derived from protection pairs.
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
    std::vector<CoverCandidate> covers;
    for (const auto& pair : state.protection_pairs) {
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

        std::string output_type = "pca";
        for (const auto& r : state.relay_outputs)
            if (r.id == pair.element0 || r.id == pair.element1) {
                output_type = "relay"; break;
            }

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
//   _sensor_id      – clarifies why type was set to "sensor_output"
//   _protection_index – links cover output to its protection pair
// Removed: _state, _turn_off, _turn_on, _sensor_on, _sensor_off,
//          _temperature_c, _humidity_pct, _energy_kwh, _power_kw,
//          _voltage_v, _current_a  (all are stale snapshots)

static json::object make_pca_entry(const myio::PcaOutput& p,
                                    const std::string& type_override = "") {
    json::object o;
    o["id"]      = p.id;
    o["name"]    = p.name;
    o["type"]    = type_override.empty()
                     ? (p.sensor_controlled ? "sensor_output"
                        : p.is_pwm          ? "light"
                                            : "switch")
                     : type_override;
    o["enabled"] = false;
    // Keep only diagnostics that aid configuration decisions
    if (p.is_pwm)            o["_pwm"]       = true;
    if (p.sensor_controlled) o["_sensor_id"] = p.sensor_id;
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
    if (r.sensor_controlled) o["_sensor_id"] = r.sensor_id;
    return o;
}

static json::object make_pwm_entry(const myio::PwmOutput& p) {
    json::object o;
    o["id"]      = p.id;
    o["name"]    = p.name;
    o["type"]    = p.sensor_controlled ? "sensor_output" : "light";
    o["enabled"] = false;
    if (p.sensor_controlled) o["_sensor_id"] = p.sensor_id;
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
    const std::unordered_map<int, std::string>& name_map)
{
    json::array covers;
    for (const auto& c : candidates) {
        auto get_name = [&](int id) {
            auto it = name_map.find(id);
            return (it != name_map.end() && !it->second.empty())
                     ? it->second : std::format("id_{}", id);
        };
        json::object cover;
        cover["id"]      = std::format("cover_{}", c.protection_index);
        cover["name"]    = get_name(c.up_id);  // name suggestion from UP output
        cover["enabled"] = false;
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
    if (covers.empty()) {
        covers.push_back(json::object{
            {"_comment",  "No cover pairs auto-detected. Fill in manually."},
            {"id",        "cover_example"},
            {"name",      "Example shutter"},
            {"enabled",   false},
            {"up",   json::object{{"type", "pca"}, {"id", 0}}},
            {"down", json::object{{"type", "pca"}, {"id", 0}}}
        });
    }
    return covers;
}

static json::object make_server_entry(const InputServer& is,
                                       const myio::ServerState& state) {
    const auto name_map      = build_name_map(state);
    const auto cover_cands   = detect_covers(state, name_map);

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
    srv["poll_interval_sec"] = 2;

    // PCA outputs – cover endpoints get "cover_up" / "cover_down" type
    json::array pca_arr;
    for (const auto& p : state.pca_outputs) {
        std::string type_override;
        for (const auto& c : cover_cands) {
            if (p.id == c.up_id)   { type_override = "cover_up";   break; }
            if (p.id == c.down_id) { type_override = "cover_down"; break; }
        }
        pca_arr.push_back(make_pca_entry(p, type_override));
    }
    srv["pca"] = std::move(pca_arr);

    // Relay outputs – same cover tagging
    json::array relay_arr;
    for (const auto& r : state.relay_outputs) {
        std::string type_override;
        for (const auto& c : cover_cands) {
            if (r.id == c.up_id)   { type_override = "cover_up";   break; }
            if (r.id == c.down_id) { type_override = "cover_down"; break; }
        }
        relay_arr.push_back(make_relay_entry(r, type_override));
    }
    srv["relays"] = std::move(relay_arr);

    json::array pwm_arr;
    for (const auto& p : state.pwm_outputs)
        pwm_arr.push_back(make_pwm_entry(p));
    srv["pwm"] = std::move(pwm_arr);

    json::array sensor_arr;
    for (const auto& s : state.sensors)
        sensor_arr.push_back(make_sensor_entry(s));
    srv["sensors"] = std::move(sensor_arr);

    srv["covers"] = make_cover_array(cover_cands, name_map);
    return srv;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const std::string input_path  = (argc > 1) ? argv[1] : "discovery_input.json";
    const std::string output_path = (argc > 2) ? argv[2] : "bridge.json";

    myio::log("[discovery] Input:  {}", input_path);
    myio::log("[discovery] Output: {}", output_path);

    std::vector<InputServer> servers;
    try {
        servers = read_input(input_path);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[discovery] Input error: {}", ex.what());
        return EXIT_FAILURE;
    }

    if (servers.empty()) {
        myio::log(stderr, "[discovery] No servers in input file.");
        return EXIT_FAILURE;
    }

    json::object root;
    root["mqtt"] = json::object{
        {"host",         "192.168.1.100"},
        {"port",         1883},
        {"client_id",    "myio_bridge"},
        {"user",         ""},
        {"password",     ""},
        {"topic_prefix", "myio"}
    };

    json::array servers_arr;
    bool any_failed = false;

    for (const auto& is : servers) {
        myio::log("[discovery] Querying: {} ({}:{}) ...", is.id, is.host, is.port);

        myio::HttpConfig hcfg;
        hcfg.host      = is.host;
        hcfg.port      = is.port;
        hcfg.user      = is.user;
        hcfg.password  = is.password;
        hcfg.server_id = is.id;

        try {
            const std::string body  = myio::http_get(hcfg, "/d_sens_out.json");
            const auto        state = myio::DSensOutParser::parse(body, is.id);

            myio::log("[discovery]   PCA outputs:      {}", state.pca_outputs.size());
            myio::log("[discovery]   Relay outputs:    {}", state.relay_outputs.size());
            myio::log("[discovery]   PWM outputs:      {}", state.pwm_outputs.size());
            myio::log("[discovery]   Sensors:          {}", state.sensors.size());
            myio::log("[discovery]   Protection pairs: {} → cover candidates",
                      state.protection_pairs.size());

            servers_arr.push_back(make_server_entry(is, state));

        } catch (const std::exception& ex) {
            myio::log(stderr, "[discovery] ERROR ({}): {}", is.id, ex.what());
            any_failed = true;
            servers_arr.push_back(json::object{
                {"id",     is.id},
                {"host",   is.host},
                {"_error", ex.what()}
            });
        }
    }

    root["servers"] = std::move(servers_arr);

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
