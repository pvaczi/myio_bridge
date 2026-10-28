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

// TCP port: reject values that would silently wrap in the uint16_t cast.
static std::uint16_t port_or(const json::object& o, std::string_view key, int fallback,
                             std::string_view ctx) {
    const int p = int_or(o, key, fallback);
    if (p < 1 || p > 65535)
        throw std::runtime_error(std::format("{}: '{}' must be 1..65535 (got {})", ctx, key, p));
    return static_cast<std::uint16_t>(p);
}

// Values used as one MQTT topic level: a '/' splits the level (every command
// topic then fails to match and is silently dropped), '+'/'#' are wildcards.
static void require_topic_level(std::string_view what, const std::string& v) {
    if (v.empty() || v.find_first_of("/+#") != std::string::npos)
        throw std::runtime_error(std::format(
            "{} '{}' must be non-empty and must not contain '/', '+' or '#'", what, v));
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

// Optional "writable" (bool, default true) of an output or cover.
static bool writable_or_true(const json::object& o, std::string_view ctx) {
    const auto* v = o.if_contains("writable");
    if (!v) return true;
    if (!v->is_bool())
        throw std::runtime_error(std::format("{}: 'writable' must be true or false", ctx));
    return v->get_bool();
}

// Optional "writable" / "device_class" keys of a PCA/relay/PWM output.
template <typename Output>
static void parse_output_access(const json::object& o, std::string_view list, Output& out,
                                std::string_view server_ctx) {
    const auto ctx = std::format("{}: {} {}", server_ctx, list, out.id);
    out.writable = writable_or_true(o, ctx);
    // A cover is driven through its endpoints: a read-only endpoint would make
    // the cover silently uncontrollable, so it is a configuration error.
    if (!out.writable &&
        (out.type == OutputType::CoverUp || out.type == OutputType::CoverDown))
        throw std::runtime_error(std::format(
            "{}: a cover endpoint cannot be 'writable': false", ctx));
    if (const auto* v = o.if_contains("device_class")) {
        if (!v->is_string() || v->get_string().empty())
            throw std::runtime_error(std::format(
                "{}: 'device_class' must be a non-empty string", ctx));
        out.device_class = std::string(v->get_string());
    }
}

// ── MQTT config ───────────────────────────────────────────────────────────────

static MqttConfig parse_mqtt(const json::object& o) {
    MqttConfig m;
    m.host         = str_or(o, "host",         "localhost");
    m.port         = port_or(o, "port", 1883, "mqtt");
    m.client_id    = str_or(o, "client_id",    "myio_bridge");
    m.user         = str_or(o, "user");
    m.password     = str_or(o, "password");
    m.topic_prefix = str_or(o, "topic_prefix", "myio");
    require_topic_level("mqtt.topic_prefix", m.topic_prefix);
    return m;
}

// ── Server config ─────────────────────────────────────────────────────────────

static CoverEndpoint parse_endpoint(const json::object& o, std::string_view ctx) {
    CoverEndpoint ep;
    ep.output_type = str_or(o, "type", "pca");
    ep.id          = int_or(o, "id");
    // Anything but "relay" is sent as a PCA command → a typo would drive the
    // wrong output, so only the two known types are accepted.
    if (ep.output_type != "pca" && ep.output_type != "relay")
        throw std::runtime_error(std::format(
            "{}: type must be \"pca\" or \"relay\" (got \"{}\")", ctx, ep.output_type));
    return ep;
}

static ServerConfig parse_server(const json::object& o) {
    ServerConfig sc;
    sc.server_id         = str_or(o, "id");
    sc.host              = str_or(o, "host");
    sc.user              = str_or(o, "user",     "admin");
    sc.password          = str_or(o, "password", "admin");
    sc.poll_interval_sec     = int_or   (o, "poll_interval_sec",    5);
    sc.sensor_log_threshold  = double_or(o, "sensor_log_threshold", 0.1);
    sc.log_startup_sync      = bool_or  (o, "log_startup_sync",     false);
    sc.coalesce_window_ms    = int_or   (o, "coalesce_window_ms",   5);
    if (const auto* v = o.if_contains("log_debug")) {
        if (!v->is_bool())
            throw std::runtime_error(std::format(
                "Server '{}': 'log_debug' must be true or false", sc.server_id));
        sc.log_debug = v->get_bool();
    }

    if (sc.server_id.empty() || sc.host.empty())
        throw std::runtime_error("Server entry requires 'id' and 'host'");
    require_topic_level("Server id", sc.server_id);
    // <prefix>/bridge/availability is the bridge's own availability topic (LWT).
    if (sc.server_id == "bridge")
        throw std::runtime_error("Server id 'bridge' is reserved");
    const auto ctx = std::format("Server '{}'", sc.server_id);
    sc.port = port_or(o, "port", 80, ctx);
    // Before fw 3.12 port 80 serves one connection at a time: a zero/negative
    // interval turns the poll loop into a request storm that starves other clients.
    if (sc.poll_interval_sec <= 0)
        throw std::runtime_error(std::format(
            "{}: poll_interval_sec must be > 0 (got {})", ctx, sc.poll_interval_sec));
    if (sc.coalesce_window_ms < 0 || sc.coalesce_window_ms > 10000)
        throw std::runtime_error(std::format(
            "{}: coalesce_window_ms must be 0..10000 (got {})", ctx, sc.coalesce_window_ms));

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
            parse_output_access(e, "pca", pc, ctx);
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
            parse_output_access(e, "relay", rc, ctx);
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
            parse_output_access(e, "pwm", pw, ctx);
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

            if (!cc.cover_id.empty())
                require_topic_level(std::format("{}: cover id", ctx), cc.cover_id);

            // Both endpoints are mandatory: without them the endpoint id would
            // be meaningless and the cover would drive an arbitrary output.
            const auto cov_ctx = std::format("{}: cover '{}'", ctx, cc.cover_id);
            // A read-only cover is meaningless (it would be a stuck motor
            // display) — reject it instead of guessing a representation.
            if (!writable_or_true(e, cov_ctx))
                throw std::runtime_error(std::format(
                    "{}: a cover cannot be 'writable': false", cov_ctx));
            const auto* up   = e.if_contains("up");
            const auto* down = e.if_contains("down");
            if (!up || !up->is_object() || !down || !down->is_object())
                throw std::runtime_error(std::format(
                    "{}: requires both 'up' and 'down' endpoint objects", cov_ctx));
            cc.up   = parse_endpoint(up->as_object(),   cov_ctx + " up");
            cc.down = parse_endpoint(down->as_object(), cov_ctx + " down");

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
    return parse_bridge_config(content);
}

BridgeConfig parse_bridge_config(std::string_view content) {
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

    // Checked last, so a broken server entry reports its own, more specific error.
    if (cfg.servers.empty())
        throw std::runtime_error("bridge.json: 'servers' must list at least one server");
    // Without it the bridge would silently publish to localhost:1883.
    if (!obj.contains("mqtt"))
        throw std::runtime_error("bridge.json: missing 'mqtt' section");

    return cfg;
}

} // namespace config
