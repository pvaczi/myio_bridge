#include "bridge_rules.hpp"

#include <boost/json/serialize.hpp>

#include <charconv>
#include <format>
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

} // namespace bridge
