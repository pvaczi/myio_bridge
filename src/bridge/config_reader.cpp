#include "config_reader.hpp"

#include <boost/json.hpp>

#include <format>
#include <fstream>
#include <stdexcept>
#include <string>

namespace config {

namespace json = boost::json;

// ── Helper functions ──────────────────────────────────────────────────────────

static std::string str_or(const json::object& o, std::string_view key,
                           std::string_view fallback = "") {
    if (const auto* v = o.if_contains(key))
        if (const auto* s = v->if_string())
            return {s->begin(), s->end()};
    return std::string(fallback);
}

static int int_or(const json::object& o, std::string_view key, int fallback = 0) {
    if (const auto* v = o.if_contains(key)) {
        if (const auto* n = v->if_int64())  return static_cast<int>(*n);
        if (const auto* n = v->if_uint64()) return static_cast<int>(*n);
    }
    return fallback;
}

static bool bool_or(const json::object& o, std::string_view key, bool fallback = false) {
    if (const auto* v = o.if_contains(key))
        if (const auto* b = v->if_bool()) return *b;
    return fallback;
}

static double double_or(const json::object& o, std::string_view key, double fallback = 0.0) {
    if (const auto* v = o.if_contains(key)) {
        if (const auto* d = v->if_double())  return *d;
        if (const auto* n = v->if_int64())   return static_cast<double>(*n);
        if (const auto* n = v->if_uint64())  return static_cast<double>(*n);
    }
    return fallback;
}

static OutputType parse_output_type(const json::object& o) {
    const auto t = str_or(o, "type", "switch");
    if (t == "light")         return OutputType::Light;
    if (t == "number")        return OutputType::Number;
    if (t == "sensor_output") return OutputType::SensorOutput;
    if (t == "cover_up")      return OutputType::CoverUp;
    if (t == "cover_down")    return OutputType::CoverDown;
    return OutputType::Switch;
}

// ── MQTT config ───────────────────────────────────────────────────────────────

static MqttConfig parse_mqtt(const json::object& o) {
    MqttConfig m;
    m.host         = str_or(o, "host",         "localhost");
    m.port         = static_cast<std::uint16_t>(int_or(o, "port", 1883));
    m.client_id    = str_or(o, "client_id",    "myio_bridge");
    m.user         = str_or(o, "user");
    m.password     = str_or(o, "password");
    m.topic_prefix = str_or(o, "topic_prefix", "myio");
    return m;
}

// ── Server config ─────────────────────────────────────────────────────────────

static CoverEndpoint parse_endpoint(const json::object& o) {
    CoverEndpoint ep;
    ep.output_type = str_or(o, "type", "pca");
    ep.id          = int_or(o, "id");
    return ep;
}

static ServerConfig parse_server(const json::object& o) {
    ServerConfig sc;
    sc.server_id         = str_or(o, "id");
    sc.host              = str_or(o, "host");
    sc.port              = static_cast<std::uint16_t>(int_or(o, "port", 80));
    sc.user              = str_or(o, "user",     "admin");
    sc.password          = str_or(o, "password", "admin");
    sc.poll_interval_sec     = int_or   (o, "poll_interval_sec",    2);
    sc.sensor_log_threshold  = double_or(o, "sensor_log_threshold", 0.1);
    sc.log_startup_sync      = bool_or  (o, "log_startup_sync",     false);

    if (sc.server_id.empty() || sc.host.empty())
        throw std::runtime_error("Server entry requires 'id' and 'host'");

    // Load ALL PCA entries regardless of enabled flag.
    // The bridge sends undiscovery (empty retained message) for disabled entries
    // so that Home Assistant removes them from its entity registry.
    if (const auto* arr = o.if_contains("pca"))
        for (const auto& item : arr->as_array()) {
            const auto& e = item.as_object();
            PcaConfig pc;
            pc.id      = int_or(e, "id");
            pc.name    = str_or(e, "name");
            pc.type    = parse_output_type(e);
            pc.enabled = bool_or(e, "enabled");
            if (pc.id > 0) sc.pca.push_back(std::move(pc));
        }

    if (const auto* arr = o.if_contains("relays"))
        for (const auto& item : arr->as_array()) {
            const auto& e = item.as_object();
            RelayConfig rc;
            rc.id      = int_or(e, "id");
            rc.name    = str_or(e, "name");
            rc.type    = parse_output_type(e);
            rc.enabled = bool_or(e, "enabled");
            if (rc.id > 0) sc.relays.push_back(std::move(rc));
        }

    if (const auto* arr = o.if_contains("pwm"))
        for (const auto& item : arr->as_array()) {
            const auto& e = item.as_object();
            PwmConfig pw;
            pw.id      = int_or(e, "id");
            pw.name    = str_or(e, "name");
            pw.type    = parse_output_type(e);
            pw.enabled = bool_or(e, "enabled");
            if (pw.id > 0) sc.pwm.push_back(std::move(pw));
        }

    if (const auto* arr = o.if_contains("sensors"))
        for (const auto& item : arr->as_array()) {
            const auto& e = item.as_object();
            SensorConfig sen;
            sen.id            = int_or   (e, "id");
            sen.name          = str_or   (e, "name");
            sen.enabled       = bool_or  (e, "enabled");
            sen.log_threshold = double_or(e, "log_threshold", -1.0);
            if (sen.id > 0) sc.sensors.push_back(std::move(sen));
        }

    if (const auto* arr = o.if_contains("covers"))
        for (const auto& item : arr->as_array()) {
            const auto& e = item.as_object();
            if (e.contains("_comment")) continue;

            CoverConfig cc;
            cc.cover_id          = str_or(e, "id");
            cc.name              = str_or(e, "name");
            cc.enabled           = bool_or(e, "enabled");
            cc.motor_timeout_sec = int_or(e, "motor_timeout_sec", 90);

            if (const auto* up   = e.if_contains("up"))
                cc.up   = parse_endpoint(up->as_object());
            if (const auto* down = e.if_contains("down"))
                cc.down = parse_endpoint(down->as_object());

            if (!cc.cover_id.empty() && (cc.up.id > 0 || !cc.enabled))
                sc.covers.push_back(std::move(cc));
        }

    return sc;
}

// ── Main loader ───────────────────────────────────────────────────────────────

BridgeConfig load_bridge_config(std::string_view path) {
    std::ifstream f{std::string(path)};
    if (!f)
        throw std::runtime_error(std::format("Cannot open config file: {}", path));

    const std::string content{std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>()};
    json::value root;
    try {
        root = json::parse(content);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("bridge.json parse error: {}", e.what()));
    }

    const auto& obj = root.as_object();
    BridgeConfig cfg;

    if (const auto* mqtt = obj.if_contains("mqtt"))
        cfg.mqtt = parse_mqtt(mqtt->as_object());

    if (const auto* servers = obj.if_contains("servers"))
        for (const auto& srv : servers->as_array())
            cfg.servers.push_back(parse_server(srv.as_object()));

    return cfg;
}

} // namespace config
