#include "bridge_rules.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

namespace bridge {

namespace json = boost::json;

namespace {

constexpr bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back()))  s.remove_suffix(1);
    return s;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

} // namespace

std::string_view command_component(OutputKind kind, config::OutputType type) {
    using config::OutputType;
    switch (kind) {
        case OutputKind::Pca:
            switch (type) {
                case OutputType::Light:     return "light";
                case OutputType::Number:    return "number";
                case OutputType::CoverUp:
                case OutputType::CoverDown: return "";
                default:                    return "switch";  // Switch, SensorOutput
            }
        case OutputKind::Relay:
            // Relays are plain on/off outputs: every non-cover type is a switch.
            if (type == OutputType::CoverUp || type == OutputType::CoverDown) return "";
            return "switch";
        case OutputKind::Pwm:
            // Legacy PWM: number or (dimmable) light — matches the discovery published so far.
            return type == OutputType::Number ? "number" : "light";
    }
    return "";
}

std::string_view ha_component(OutputKind kind, config::OutputType type, bool writable) {
    const auto cmd = command_component(kind, type);
    if (writable || cmd.empty()) return cmd;
    if (cmd == "switch") return "binary_sensor";
    return "sensor";  // light, number → percentage sensor
}

const config::CoverConfig* find_enabled_cover(const std::vector<config::CoverConfig>& covers,
                                              OutputKind kind, int id) {
    if (kind == OutputKind::Pwm) return nullptr;
    const std::string_view t = (kind == OutputKind::Relay) ? "relay" : "pca";
    for (const auto& c : covers) {
        if (!c.enabled) continue;
        if ((c.up.output_type   == t && c.up.id   == id) ||
            (c.down.output_type == t && c.down.id == id))
            return &c;
    }
    return nullptr;
}

bool is_cover_endpoint(const std::vector<config::CoverConfig>& covers, OutputKind kind, int id) {
    return find_enabled_cover(covers, kind, id) != nullptr;
}

std::optional<bool> parse_on_off(std::string_view payload) {
    const auto p = upper(trim(payload));
    if (p == "ON")  return true;
    if (p == "OFF") return false;
    return std::nullopt;
}

std::optional<int> parse_int_in_range(std::string_view payload, int lo, int hi) {
    const auto p = trim(payload);
    if (p.empty()) return std::nullopt;
    int value = 0;
    const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
    if (ec != std::errc{} || ptr != p.data() + p.size()) return std::nullopt;
    if (value < lo || value > hi) return std::nullopt;
    return value;
}

int percent_to_raw(int percent) {
    return (percent * 255 + 50) / 100;  // round half up, non-negative input
}

int raw_to_percent(int raw) {
    return (raw * 100 + 127) / 255;     // round half up, non-negative input
}

std::optional<CoverCommand> parse_cover_command(std::string_view payload) {
    const auto p = upper(trim(payload));
    if (p == "OPEN")  return CoverCommand::Open;
    if (p == "CLOSE") return CoverCommand::Close;
    if (p == "STOP")  return CoverCommand::Stop;
    return std::nullopt;
}

std::optional<int> parse_http_status(std::string_view raw) {
    // Status line only: a search over the whole response would misfire on
    // header values such as "Content-Length: 1401".
    const auto eol = raw.find('\n');
    std::string_view line = raw.substr(0, eol);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (!line.starts_with("HTTP/")) return std::nullopt;
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    line.remove_prefix(sp);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);

    // Exactly three digits, followed by end of line or a separator.
    if (line.size() < 3) return std::nullopt;
    for (std::size_t i = 0; i < 3; ++i)
        if (line[i] < '0' || line[i] > '9') return std::nullopt;
    if (line.size() > 3 && line[3] != ' ' && line[3] != '\t') return std::nullopt;
    const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (code < 100) return std::nullopt;
    return code;
}

PostVerdict classify_post_status(std::optional<int> status) {
    if (!status) return PostVerdict::NoStatus;
    if (*status == 401) return PostVerdict::Unauthorized;
    if (*status < 200 || *status > 399) return PostVerdict::Rejected;
    return PostVerdict::Accepted;
}

std::string extract_json_body(const std::string& raw, std::string_view host) {
    // A 401 is recognised from the status line first: its header block is
    // complete and its HTML body has no JSON, so it must not fall through to
    // the generic "No JSON found" error.
    if (parse_http_status(raw) == 401)
        throw std::runtime_error(
            std::format("[{}] HTTP 401 – wrong username or password", host));

    // Find the end of the HTTP header block (\r\n\r\n or bare \n\n).
    std::size_t body_start = std::string::npos;
    if (const auto pos = raw.find("\r\n\r\n"); pos != std::string::npos)
        body_start = pos + 4;
    else if (const auto pos2 = raw.find("\n\n"); pos2 != std::string::npos)
        body_start = pos2 + 2;

    if (body_start == std::string::npos) {
        if (raw.find("401") != std::string::npos)
            throw std::runtime_error(
                std::format("[{}] HTTP 401 – wrong username or password", host));
        throw std::runtime_error(
            std::format("[{}] No HTTP header terminator found in response", host));
    }

    const auto start = raw.find('{', body_start);
    if (start == std::string::npos)
        throw std::runtime_error(
            std::format("[{}] No JSON found in response body", host));

    const auto end = raw.rfind('}');
    if (end == std::string::npos || end < start)
        throw std::runtime_error(
            std::format("[{}] Malformed JSON in response", host));

    return raw.substr(start, end - start + 1);
}

std::string extract_http_body(const std::string& raw, std::string_view host) {
    const auto status = parse_http_status(raw);
    if (status == 401)
        throw std::runtime_error(
            std::format("[{}] HTTP 401 – wrong username or password", host));

    // End of the header block: \r\n\r\n or bare \n\n (whichever comes first).
    const auto crlf = raw.find("\r\n\r\n");
    const auto lf   = raw.find("\n\n");
    std::size_t body_start = std::string::npos;
    if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf))
        body_start = crlf + 4;
    else if (lf != std::string::npos)
        body_start = lf + 2;
    if (body_start == std::string::npos)
        throw std::runtime_error(
            std::format("[{}] No HTTP header terminator found in response", host));

    if (status && (*status < 200 || *status > 299))
        throw std::runtime_error(std::format("[{}] HTTP {}", host, *status));

    return raw.substr(body_start);
}

std::vector<std::string> chunk_post_fragments(const std::vector<std::string>& fragments,
                                              std::size_t max_len) {
    std::vector<std::string> chunks;
    for (const auto& f : fragments) {
        if (f.empty()) continue;
        // '&' separator costs one char; an oversized fragment goes alone.
        if (!chunks.empty() && chunks.back().size() + 1 + f.size() <= max_len)
            chunks.back() += '&' + f;
        else
            chunks.push_back(f);
    }
    return chunks;
}

std::string bridge_availability_topic(std::string_view prefix) {
    return std::format("{}/bridge/availability", prefix);
}

std::string server_availability_topic(std::string_view prefix, std::string_view server_id) {
    return std::format("{}/{}/availability", prefix, server_id);
}

json::array availability_list(std::string_view prefix, std::string_view server_id) {
    return {
        json::object{{"topic", bridge_availability_topic(prefix)}},
        json::object{{"topic", server_availability_topic(prefix, server_id)}},
    };
}

SensorClass sensor_class(int id) {
    // Id ranges as parsed in dsens_parser.cpp (and IO Valet 3.9 API doc:
    // temperature sensor ids < 100, humidity sensor id = 100 + n).
    if (id >= 1   && id <= 100) return {"temperature", "°C", "measurement"};
    if (id >= 101 && id <= 108) return {"humidity",    "%",  "measurement"};
    if (id >= 201 && id <= 203) return {"power",       "kW", "measurement"};
    if (id >= 204 && id <= 206) return {"voltage",     "V",  "measurement"};
    if (id >= 207 && id <= 209) return {"current",     "A",  "measurement"};
    // 200 = impulse meter: its unit is user-defined on the device (max 5 chars)
    // and the value is an average, not a running total → no device_class/unit.
    return {"", "", "measurement"};
}

json::object origin_block(std::string_view bridge_version) {
    return {
        {"name",       "myio-bridge"},
        {"sw_version", bridge_version},
    };
}

json::object server_device_block(const DiscoveryContext& ctx) {
    json::object d{
        {"identifiers",  json::array{std::format("myio_{}", ctx.server_id)}},
        {"name",         std::format("myIO {}", ctx.server_id)},
        {"model",        "IO Valet"},
        {"manufacturer", "myIO"},
    };
    if (!ctx.firmware_version.empty()) d["sw_version"] = ctx.firmware_version;
    d["configuration_url"] = (ctx.port == 80)
        ? std::format("http://{}/", ctx.host)
        : std::format("http://{}:{}/", ctx.host, ctx.port);
    d["via_device"] = kBridgeDeviceId;
    return d;
}

json::object bridge_device_block(std::string_view bridge_version) {
    return {
        {"identifiers",  json::array{kBridgeDeviceId}},
        {"name",         "myIO bridge"},
        {"model",        "myio-bridge"},
        {"manufacturer", "myio-bridge"},
        {"sw_version",   bridge_version},
    };
}

void add_entity_common(json::object& o, const DiscoveryContext& ctx,
                       std::string_view component, std::string_view object_id) {
    o["default_entity_id"] = std::format("{}.{}", component, object_id);
    o["availability"]      = availability_list(ctx.prefix, ctx.server_id);
    o["availability_mode"] = "all";
    o["device"]            = server_device_block(ctx);
    o["origin"]            = origin_block(ctx.bridge_version);
}

std::string bridge_status_discovery_topic() {
    return std::format("homeassistant/binary_sensor/{}_connectivity/config", kBridgeDeviceId);
}

std::string make_bridge_status_discovery(std::string_view prefix,
                                         std::string_view bridge_version) {
    // No availability: the entity itself shows the bridge's online/offline state.
    // name null → HA names it after the device class ("Connectivity").
    json::object o{
        {"name",              nullptr},
        {"unique_id",         std::format("{}_connectivity", kBridgeDeviceId)},
        {"default_entity_id", std::format("binary_sensor.{}_connectivity", kBridgeDeviceId)},
        {"state_topic",       bridge_availability_topic(prefix)},
        {"payload_on",        kAvailabilityOnline},
        {"payload_off",       kAvailabilityOffline},
        {"device_class",      "connectivity"},
        {"entity_category",   "diagnostic"},
        {"device",            bridge_device_block(bridge_version)},
        {"origin",            origin_block(bridge_version)},
    };
    return json::serialize(o);
}

namespace {
struct ButtonMeta {
    std::string_view action;  // command topic level and unique_id suffix
    std::string_view name;    // HA entity name (shown to the user)
    std::string_view icon;
};

ButtonMeta button_meta(BridgeButton b) {
    switch (b) {
        case BridgeButton::Rediscover: return {"rediscover", "Discovery újraküldése",       "mdi:broadcast"};
        case BridgeButton::Resync:     return {"resync",     "Állapot újraszinkronizálása", "mdi:sync"};
    }
    return {"", "", ""};
}
} // namespace

std::string bridge_button_command_topic(std::string_view prefix, BridgeButton b) {
    return std::format("{}/bridge/{}", prefix, button_meta(b).action);
}

std::string bridge_button_discovery_topic(BridgeButton b) {
    return std::format("homeassistant/button/{}_{}/config", kBridgeDeviceId, button_meta(b).action);
}

std::string make_bridge_button_discovery(std::string_view prefix,
                                         std::string_view bridge_version, BridgeButton b) {
    const auto m = button_meta(b);
    json::object o{
        {"name",              m.name},
        {"unique_id",         std::format("{}_{}", kBridgeDeviceId, m.action)},
        {"default_entity_id", std::format("button.{}_{}", kBridgeDeviceId, m.action)},
        {"command_topic",     bridge_button_command_topic(prefix, b)},
        {"payload_press",     kButtonPress},
        {"availability_topic", bridge_availability_topic(prefix)},
        {"entity_category",   "config"},
        {"icon",              m.icon},
        {"device",            bridge_device_block(bridge_version)},
        {"origin",            origin_block(bridge_version)},
    };
    return json::serialize(o);
}

std::optional<BridgeButton> bridge_button_pressed(std::string_view prefix,
                                                  std::string_view topic,
                                                  std::string_view payload) {
    if (payload != kButtonPress) return std::nullopt;
    for (const auto b : kBridgeButtons)
        if (topic == bridge_button_command_topic(prefix, b)) return b;
    return std::nullopt;
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

std::string format_iso_utc(std::chrono::system_clock::time_point t) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(t));
}

std::string server_diag_topic(std::string_view prefix, std::string_view server_id) {
    return std::format("{}/{}/diag", prefix, server_id);
}

namespace {
json::object server_diag_object(const ServerDiag& d) {
    return json::object{
        {"last_poll_ok",       d.last_poll_ok ? json::value(format_iso_utc(*d.last_poll_ok))
                                              : json::value(nullptr)},
        {"poll_latency_ms",    d.poll_latency_ms ? json::value(*d.poll_latency_ms)
                                                 : json::value(nullptr)},
        {"consecutive_errors", d.consecutive_errors},
        {"total_errors",       d.total_errors},
        {"last_error",         d.last_error},
        {"firmware",           d.firmware},
        {"info_error",         d.info_error},
    };
}
} // namespace

std::string make_server_diag_state(const ServerDiag& d) {
    return json::serialize(server_diag_object(d));
}

std::string make_server_status_state(const ServerDiag& d, std::optional<bool> online,
                                     std::chrono::system_clock::time_point now,
                                     const std::optional<SlotInfo>& slot) {
    auto o = server_diag_object(d);
    o["online"]     = online ? json::value(*online) : json::value(nullptr);
    o["written_at"] = format_iso_utc(now);
    if (slot) {
        json::object s;
        s["active"] = json::object{{"index", slot->index}, {"name", slot->name}};
        if (slot->boot_index)
            s["boot"] = json::object{{"index", *slot->boot_index}, {"name", slot->boot_name}};
        o["slot"] = std::move(s);
    }
    return json::serialize(o);
}

void mqtt_status_connected(MqttStatus& s, MqttStatus::time_point now) {
    if (s.last_connect) ++s.reconnects;
    s.connected    = true;
    s.since        = now;
    s.last_connect = now;
}

void mqtt_status_disconnected(MqttStatus& s, MqttStatus::time_point now, std::string error) {
    if (s.connected) {
        s.connected       = false;
        s.since           = now;
        s.last_disconnect = now;
    }
    if (!error.empty()) s.last_error = std::move(error);
}

std::string make_mqtt_status_state(const MqttStatus& s, std::chrono::system_clock::time_point now) {
    const auto ts = [](const std::optional<MqttStatus::time_point>& t) {
        return t ? json::value(format_iso_utc(*t)) : json::value(nullptr);
    };
    json::object o{
        {"connected",       s.connected},
        {"broker",          s.broker},
        {"since",           ts(s.since)},
        {"last_connect",    ts(s.last_connect)},
        {"last_disconnect", ts(s.last_disconnect)},
        {"reconnects",      s.reconnects},
        {"last_error",      s.last_error.empty() ? json::value(nullptr) : json::value(s.last_error)},
        {"written_at",      format_iso_utc(now)},
    };
    return json::serialize(o);
}

namespace {
struct DiagMeta {
    std::string_view key;       // unique_id / topic suffix and diag JSON field group
    std::string_view name;      // HA entity name (shown to the user)
    std::string_view icon;      // "" = none
    std::string_view value_template;
    std::string_view device_class;  // "" = none
    std::string_view unit;          // "" = none
    std::string_view state_class;   // "" = none
};

DiagMeta diag_meta(DiagEntity e) {
    switch (e) {
        case DiagEntity::LastPoll:
            return {"last_poll", "Utolsó sikeres poll", "",
                    "{{ value_json.last_poll_ok }}", "timestamp", "", ""};
        case DiagEntity::PollLatency:
            return {"poll_latency", "Poll-késleltetés", "",
                    "{{ value_json.poll_latency_ms }}", "duration", "ms", "measurement"};
        case DiagEntity::ConsecutiveErrors:
            return {"consecutive_errors", "Egymást követő poll-hibák", "mdi:alert-circle-outline",
                    "{{ value_json.consecutive_errors }}", "", "", "measurement"};
        case DiagEntity::TotalErrors:
            return {"total_errors", "Poll-hibák (indulás óta)", "mdi:counter",
                    "{{ value_json.total_errors }}", "", "", "total_increasing"};
        case DiagEntity::LastError:
            return {"last_error", "Utolsó poll-hiba", "mdi:alert",
                    "{{ value_json.last_error if value_json.last_error else 'nincs' }}", "", "", ""};
        case DiagEntity::Firmware:
            return {"firmware", "Firmware-verzió", "mdi:chip",
                    "{{ value_json.firmware if value_json.firmware else 'ismeretlen' }}", "", "", ""};
        case DiagEntity::InfoError:
            return {"info_error", "Utolsó info-hiba", "mdi:alert",
                    "{{ value_json.info_error if value_json.info_error else 'nincs' }}", "", "", ""};
    }
    return {};
}
} // namespace

std::string server_diag_discovery_topic(std::string_view server_id, DiagEntity e) {
    return std::format("homeassistant/sensor/myio_{}_diag_{}/config", server_id, diag_meta(e).key);
}

std::string make_server_diag_discovery(const DiscoveryContext& ctx, DiagEntity e) {
    const auto m = diag_meta(e);
    const std::string uid = std::format("myio_{}_diag_{}", ctx.server_id, m.key);
    json::object o{
        {"name",               m.name},
        {"unique_id",          uid},
        {"default_entity_id",  std::format("sensor.{}", uid)},
        {"state_topic",        server_diag_topic(ctx.prefix, ctx.server_id)},
        {"value_template",     m.value_template},
        // Bridge topic only: the diagnostics must stay visible while the server is offline.
        {"availability_topic", bridge_availability_topic(ctx.prefix)},
        {"entity_category",    "diagnostic"},
        {"device",             server_device_block(ctx)},
        {"origin",             origin_block(ctx.bridge_version)},
    };
    if (!m.icon.empty())         o["icon"]                = m.icon;
    if (!m.device_class.empty()) o["device_class"]        = m.device_class;
    if (!m.unit.empty())         o["unit_of_measurement"] = m.unit;
    if (!m.state_class.empty())  o["state_class"]         = m.state_class;
    return json::serialize(o);
}

// ── Thermostat thresholds ─────────────────────────────────────────────────────

namespace {

constexpr bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$';
}

std::size_t skip_spaces(std::string_view s, std::size_t i) {
    while (i < s.size() && is_space(s[i])) ++i;
    return i;
}

constexpr int kThermoMaxTempActivator = 99;   // 1..99 temperature sensor, >= 100 humidity
constexpr int kFirstRelayId = 1;              // relay array index = id - 1
constexpr int kFirstPcaId   = 2001;           // PCA array index   = id - 2001

std::string_view kind_label(OutputKind kind) {
    switch (kind) {
        case OutputKind::Pca:   return "pca";
        case OutputKind::Relay: return "relay";
        case OutputKind::Pwm:   return "pwm";
    }
    return "";
}

std::string_view which_label(ThresholdKind which) {
    return which == ThresholdKind::On ? "on" : "off";
}

} // namespace

std::optional<std::vector<int>> parse_js_int_array(std::string_view html, std::string_view name) {
    if (name.empty()) return std::nullopt;
    for (auto pos = html.find(name); pos != std::string_view::npos;
         pos = html.find(name, pos + 1)) {
        // Before the name: whitespace, then the `var` keyword as a whole word.
        // (A prefixed identifier such as PCA_<name> has '_' right before the name.)
        auto b = pos;
        while (b > 0 && is_space(html[b - 1])) --b;
        if (b == pos || b < 3 || html.substr(b - 3, 3) != "var") continue;
        if (b > 3 && is_ident_char(html[b - 4])) continue;

        // After the name: the whole identifier, then `=` and `[`.
        auto i = pos + name.size();
        if (i < html.size() && is_ident_char(html[i])) continue;
        i = skip_spaces(html, i);
        if (i >= html.size() || html[i] != '=') continue;
        i = skip_spaces(html, i + 1);
        if (i >= html.size() || html[i] != '[') continue;
        ++i;

        // The declaration is found: any irregularity in it is a malformed array.
        std::vector<int> values;
        i = skip_spaces(html, i);
        if (i < html.size() && html[i] == ']') return values;
        while (true) {
            i = skip_spaces(html, i);
            int v = 0;
            const char* first = html.data() + i;
            const char* last  = html.data() + html.size();
            const auto [ptr, ec] = std::from_chars(first, last, v);
            if (ec != std::errc{}) return std::nullopt;
            values.push_back(v);
            i = skip_spaces(html, static_cast<std::size_t>(ptr - html.data()));
            if (i >= html.size()) return std::nullopt;
            if (html[i] == ']') return values;
            if (html[i] != ',') return std::nullopt;
            ++i;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<ThermostatThreshold>>
map_thermostat_thresholds(std::string_view html,
                          const std::vector<int>& relay_ids,
                          const std::vector<int>& pca_ids) {
    std::vector<ThermostatThreshold> out;
    bool any_group = false;

    auto map_group = [&](OutputKind kind, int first_id, const std::vector<int>& ids,
                         std::string_view act_name, std::string_view on_name,
                         std::string_view off_name) {
        const auto act = parse_js_int_array(html, act_name);
        const auto on  = parse_js_int_array(html, on_name);
        const auto off = parse_js_int_array(html, off_name);
        if (!act || !on || !off) return;
        any_group = true;
        for (const int id : ids) {
            const int idx = id - first_id;
            if (idx < 0) continue;
            const auto u = static_cast<std::size_t>(idx);
            if (u >= act->size() || u >= on->size() || u >= off->size()) continue;
            const int a = (*act)[u];
            if (a < 1 || a > kThermoMaxTempActivator) continue;
            out.push_back({kind, id, a, (*on)[u], (*off)[u]});
        }
    };

    map_group(OutputKind::Relay, kFirstRelayId, relay_ids,
              "thermoActivator", "min_temp_ON", "max_temp_OFF");
    map_group(OutputKind::Pca, kFirstPcaId, pca_ids,
              "PCA_thermoActivator", "PCA_min_temp_ON", "PCA_max_temp_OFF");

    if (!any_group) return std::nullopt;
    return out;
}

std::string format_tenths(int tenths) {
    const long long v = tenths;
    const long long a = v < 0 ? -v : v;
    return std::format("{}{}.{}", v < 0 ? "-" : "", a / 10, a % 10);
}

std::string threshold_state_topic(std::string_view prefix, std::string_view server_id,
                                  OutputKind kind, int id, ThresholdKind which) {
    return std::format("{}/{}/threshold/{}/{}/{}", prefix, server_id,
                       kind_label(kind), id, which_label(which));
}

std::string threshold_discovery_topic(std::string_view server_id, OutputKind kind, int id,
                                      ThresholdKind which) {
    return std::format("homeassistant/sensor/myio_{}_{}_{}_thr_{}/config", server_id,
                       kind_label(kind), id, which_label(which));
}

std::string make_threshold_discovery(const DiscoveryContext& ctx, OutputKind kind, int id,
                                     std::string_view output_name, ThresholdKind which) {
    const std::string base = std::format("myio_{}_{}_{}", ctx.server_id, kind_label(kind), id);
    const std::string uid  = std::format("{}_thr_{}", base, which_label(which));
    const std::string_view label = which == ThresholdKind::On ? "bekapcsolási küszöb"
                                                              : "kikapcsolási küszöb";
    json::object o;
    o["name"]                      = std::format("{} {}", output_name.empty() ? base : std::string(output_name), label);
    o["unique_id"]                 = uid;
    o["state_topic"]               = threshold_state_topic(ctx.prefix, ctx.server_id, kind, id, which);
    o["device_class"]              = "temperature";
    o["unit_of_measurement"]       = "°C";
    o["suggested_display_precision"] = 1;
    o["entity_category"]           = "diagnostic";
    add_entity_common(o, ctx, "sensor", uid);
    return json::serialize(o);
}

// ── Writable thermostat thresholds ───────────────────────────────────────────

std::string threshold_command_topic(std::string_view prefix, std::string_view server_id,
                                    OutputKind kind, int id, ThresholdKind which) {
    return threshold_state_topic(prefix, server_id, kind, id, which) + "/set";
}

std::string threshold_number_discovery_topic(std::string_view server_id, OutputKind kind,
                                             int id, ThresholdKind which) {
    return std::format("homeassistant/number/myio_{}_{}_{}_thr_{}_set/config", server_id,
                       kind_label(kind), id, which_label(which));
}

std::string make_threshold_number_discovery(const DiscoveryContext& ctx, OutputKind kind,
                                            int id, std::string_view output_name,
                                            ThresholdKind which) {
    const std::string base = std::format("myio_{}_{}_{}", ctx.server_id, kind_label(kind), id);
    const std::string uid  = std::format("{}_thr_{}_set", base, which_label(which));
    const std::string_view label = which == ThresholdKind::On ? "bekapcsolási küszöb"
                                                              : "kikapcsolási küszöb";
    json::object o;
    o["name"]                = std::format("{} {} (állítás)",
                                           output_name.empty() ? base : std::string(output_name), label);
    o["unique_id"]           = uid;
    o["state_topic"]         = threshold_state_topic(ctx.prefix, ctx.server_id, kind, id, which);
    o["command_topic"]       = threshold_command_topic(ctx.prefix, ctx.server_id, kind, id, which);
    o["min"]                 = kThresholdMinC;
    o["max"]                 = kThresholdMaxC;
    o["step"]                = 0.1;
    o["mode"]                = "box";
    o["unit_of_measurement"] = "°C";
    o["device_class"]        = "temperature";
    o["entity_category"]     = "config";
    add_entity_common(o, ctx, "number", uid);
    return json::serialize(o);
}

std::string threshold_number_payload(const DiscoveryContext& ctx, OutputKind kind, int id,
                                     std::string_view output_name, ThresholdKind which,
                                     bool write_enabled) {
    if (!write_enabled) return {};
    return make_threshold_number_discovery(ctx, kind, id, output_name, which);
}

std::optional<ThresholdTarget> parse_threshold_command_topic(std::string_view topic) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto slash = topic.find('/', start);
        if (slash == std::string_view::npos) { parts.push_back(topic.substr(start)); break; }
        parts.push_back(topic.substr(start, slash - start));
        start = slash + 1;
    }
    if (parts.size() != 7 || parts[2] != "threshold" || parts[6] != "set") return std::nullopt;

    ThresholdTarget t{};
    if      (parts[3] == "relay") t.kind = OutputKind::Relay;
    else if (parts[3] == "pca")   t.kind = OutputKind::Pca;
    else return std::nullopt;

    const auto id_str = parts[4];
    if (id_str.empty()) return std::nullopt;
    const auto [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), t.id);
    if (ec != std::errc{} || ptr != id_str.data() + id_str.size() || t.id < 1)
        return std::nullopt;

    if      (parts[5] == "on")  t.which = ThresholdKind::On;
    else if (parts[5] == "off") t.which = ThresholdKind::Off;
    else return std::nullopt;
    return t;
}

std::optional<int> parse_threshold_payload(std::string_view payload) {
    std::string p(trim(payload));
    if (p.empty()) return std::nullopt;
    std::replace(p.begin(), p.end(), ',', '.');
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
    if (ec != std::errc{} || ptr != p.data() + p.size()) return std::nullopt;
    if (!std::isfinite(value) || value < kThresholdMinC || value > kThresholdMaxC)
        return std::nullopt;
    return static_cast<int>(std::lround(value * 10.0));
}

std::string make_threshold_command(const ThresholdTarget& target, int tenths) {
    const bool on = target.which == ThresholdKind::On;
    const std::string_view name = target.kind == OutputKind::Pca
        ? (on ? "PCA_temp_MIN" : "PCA_temp_MAX")
        : (on ? "min_temp_ON"  : "max_temp_OFF");
    return std::format("{}*{}={}", name, target.id, tenths);
}

// ── Active slot ───────────────────────────────────────────────────────────────

namespace {

// Offset of the value of the first `var <name> = ...` declaration (after the '=' and
// the whitespace behind it); nullopt = no such declaration. `var` and the name must
// be whole words.
std::optional<std::size_t> find_var_value(std::string_view html, std::string_view name) {
    if (name.empty()) return std::nullopt;
    for (auto pos = html.find(name); pos != std::string_view::npos;
         pos = html.find(name, pos + 1)) {
        auto b = pos;
        while (b > 0 && is_space(html[b - 1])) --b;
        if (b == pos || b < 3 || html.substr(b - 3, 3) != "var") continue;
        if (b > 3 && is_ident_char(html[b - 4])) continue;

        auto i = pos + name.size();
        if (i < html.size() && is_ident_char(html[i])) continue;
        i = skip_spaces(html, i);
        if (i >= html.size() || html[i] != '=') continue;
        return skip_spaces(html, i + 1);
    }
    return std::nullopt;
}

} // namespace

std::optional<int> parse_js_int_scalar(std::string_view html, std::string_view name) {
    const auto at = find_var_value(html, name);
    if (!at || *at >= html.size()) return std::nullopt;
    int v = 0;
    const char* first = html.data() + *at;
    const char* last  = html.data() + html.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{}) return std::nullopt;
    // `1.5` or `12abc` is not an integer literal.
    if (ptr != last && (is_ident_char(*ptr) || *ptr == '.')) return std::nullopt;
    return v;
}

std::map<int, std::string> parse_js_indexed_strings(std::string_view html, std::string_view name) {
    std::map<int, std::string> out;
    if (name.empty()) return out;
    for (auto pos = html.find(name); pos != std::string_view::npos;
         pos = html.find(name, pos + 1)) {
        if (pos > 0 && is_ident_char(html[pos - 1])) continue;
        auto i = pos + name.size();
        if (i < html.size() && is_ident_char(html[i])) continue;
        i = skip_spaces(html, i);
        if (i >= html.size() || html[i] != '[') continue;
        ++i;
        int index = 0;
        const auto [ptr, ec] = std::from_chars(html.data() + i, html.data() + html.size(), index);
        if (ec != std::errc{}) continue;
        i = static_cast<std::size_t>(ptr - html.data());
        if (i >= html.size() || html[i] != ']') continue;
        i = skip_spaces(html, i + 1);
        if (i >= html.size() || html[i] != '=') continue;
        i = skip_spaces(html, i + 1);
        if (i >= html.size() || html[i] != '\'') continue;
        ++i;

        std::string text;
        bool closed = false;
        for (; i < html.size(); ++i) {
            const char c = html[i];
            if (c == '\n' || c == '\r') break;                 // unterminated literal
            if (c == '\'') { closed = true; break; }
            if (c == '\\' && i + 1 < html.size() && html[i + 1] != '\n' && html[i + 1] != '\r') {
                const char n = html[i + 1];
                if (n != '\'' && n != '\\') text.push_back(c);  // other escapes stay verbatim
                text.push_back(n);
                ++i;
                continue;
            }
            text.push_back(c);
        }
        if (closed) out.insert_or_assign(index, std::move(text));
    }
    return out;
}

std::optional<SlotInfo> parse_active_slot(std::string_view html) {
    const auto actual = parse_js_int_scalar(html, "actualSlot");
    if (!actual || *actual < 0) return std::nullopt;
    SlotInfo info;
    info.index      = *actual;
    info.boot_index = parse_js_int_scalar(html, "bootUpSlot");
    if (const auto exist = parse_js_int_array(html, "slotExist"))
        info.count = static_cast<int>(std::count(exist->begin(), exist->end(), 1));

    const auto names = parse_js_indexed_strings(html, "slot_description");
    const auto name_of = [&](int index) {
        std::string n;
        if (const auto it = names.find(index); it != names.end())
            n = std::string(trim(it->second));
        if (n.empty()) n = std::format("Slot {}", index + 1);
        return n;
    };
    info.name = name_of(info.index);
    if (info.boot_index && *info.boot_index >= 0) info.boot_name = name_of(*info.boot_index);
    else info.boot_index.reset();
    return info;
}

std::string slot_state_topic(std::string_view prefix, std::string_view server_id) {
    return std::format("{}/{}/slot/state", prefix, server_id);
}

std::string slot_attributes_topic(std::string_view prefix, std::string_view server_id) {
    return std::format("{}/{}/slot/attributes", prefix, server_id);
}

std::string slot_discovery_topic(std::string_view server_id) {
    return std::format("homeassistant/sensor/myio_{}_active_slot/config", server_id);
}

std::string make_slot_attributes(const SlotInfo& info) {
    json::object o;
    o["index"] = info.index;
    if (info.boot_index) {
        o["boot_index"] = *info.boot_index;
        o["boot_name"]  = info.boot_name;
    }
    if (info.count)      o["count"]      = *info.count;
    return json::serialize(o);
}

std::string make_slot_discovery(const DiscoveryContext& ctx) {
    const std::string uid = std::format("myio_{}_active_slot", ctx.server_id);
    json::object o;
    o["name"]                   = "Aktív slot";
    o["unique_id"]              = uid;
    o["state_topic"]            = slot_state_topic(ctx.prefix, ctx.server_id);
    o["json_attributes_topic"]  = slot_attributes_topic(ctx.prefix, ctx.server_id);
    o["icon"]                   = "mdi:format-list-numbered";
    add_entity_common(o, ctx, "sensor", uid);
    return json::serialize(o);
}

std::string bridge_started_topic(std::string_view prefix) {
    return std::format("{}/bridge/started", prefix);
}

std::string bridge_started_discovery_topic() {
    return std::format("homeassistant/sensor/{}_started/config", kBridgeDeviceId);
}

std::string make_bridge_started_discovery(std::string_view prefix,
                                          std::string_view bridge_version) {
    json::object o{
        {"name",               "Indulás"},
        {"unique_id",          std::format("{}_started", kBridgeDeviceId)},
        {"default_entity_id",  std::format("sensor.{}_started", kBridgeDeviceId)},
        {"state_topic",        bridge_started_topic(prefix)},
        {"availability_topic", bridge_availability_topic(prefix)},
        {"device_class",       "timestamp"},
        {"entity_category",    "diagnostic"},
        {"device",             bridge_device_block(bridge_version)},
        {"origin",             origin_block(bridge_version)},
    };
    return json::serialize(o);
}

} // namespace bridge
