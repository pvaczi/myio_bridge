#include "bridge.hpp"
#include <log.hpp>
#include "async_http_client.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>

namespace bridge {

namespace json = boost::json;
using namespace std::chrono_literals;

// ── Topic generators ──────────────────────────────────────────────────────────

std::string switch_state_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/switch/{}/state", p, s, id);
}
std::string switch_cmd_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/switch/{}/set", p, s, id);
}
std::string light_state_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/light/{}/state", p, s, id);
}
std::string light_bright_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/light/{}/brightness", p, s, id);
}
std::string light_cmd_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/light/{}/set", p, s, id);
}
std::string light_bright_cmd_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/light/{}/brightness/set", p, s, id);
}
std::string cover_state_topic(const std::string& p, const std::string& s,
                               const std::string& cid) {
    return std::format("{}/{}/cover/{}/state", p, s, cid);
}
std::string cover_position_topic(const std::string& p, const std::string& s,
                                  const std::string& cid) {
    return std::format("{}/{}/cover/{}/position", p, s, cid);
}
std::string cover_cmd_topic(const std::string& p, const std::string& s,
                             const std::string& cid) {
    return std::format("{}/{}/cover/{}/set", p, s, cid);
}
std::string sensor_state_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/sensor/{}/state", p, s, id);
}
std::string number_state_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/number/{}/state", p, s, id);
}

// ── HA discovery JSON helpers ─────────────────────────────────────────────────

static json::object device_info(const std::string& srv_id) {
    return {
        {"identifiers", json::array{std::format("myio_{}", srv_id)}},
        {"name",        std::format("myIO {}", srv_id)},
        {"model",       "IO Valet"},
        {"manufacturer","myIO"}
    };
}

std::string make_switch_discovery(const std::string& p, const std::string& s,
                                   const config::PcaConfig& e) {
    const std::string uid  = std::format("myio_{}_pca_{}", s, e.id);
    const std::string base = std::format("{}/{}/switch/{}", p, s, e.id);
    json::object o;
    o["name"]          = e.name.empty() ? uid : e.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    o["payload_on"]    = "ON";
    o["payload_off"]   = "OFF";
    o["retain"]        = true;
    o["device"]        = device_info(s);
    return json::serialize(o);
}

std::string make_switch_discovery(const std::string& p, const std::string& s,
                                   const config::RelayConfig& e) {
    const std::string uid  = std::format("myio_{}_relay_{}", s, e.id);
    const std::string base = std::format("{}/{}/switch/{}", p, s, e.id);
    json::object o;
    o["name"]          = e.name.empty() ? uid : e.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    o["payload_on"]    = "ON";
    o["payload_off"]   = "OFF";
    o["retain"]        = true;
    o["device"]        = device_info(s);
    return json::serialize(o);
}

std::string make_light_discovery(const std::string& p, const std::string& s,
                                  const config::PcaConfig& e, bool brightness) {
    const std::string uid  = std::format("myio_{}_pca_{}", s, e.id);
    const std::string base = std::format("{}/{}/light/{}", p, s, e.id);
    json::object o;
    o["name"]          = e.name.empty() ? uid : e.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    if (brightness) {
        o["brightness_state_topic"]   = base + "/brightness";
        o["brightness_command_topic"] = base + "/brightness/set";
        o["brightness_scale"]         = 255;
    }
    o["payload_on"]  = "ON";
    o["payload_off"] = "OFF";
    o["retain"]      = true;
    o["device"]      = device_info(s);
    return json::serialize(o);
}

std::string make_light_discovery(const std::string& p, const std::string& s,
                                  const config::PwmConfig& e) {
    const std::string uid  = std::format("myio_{}_pwm_{}", s, e.id);
    const std::string base = std::format("{}/{}/light/{}", p, s, e.id);
    json::object o;
    o["name"]                     = e.name.empty() ? uid : e.name;
    o["unique_id"]                = uid;
    o["state_topic"]              = base + "/state";
    o["command_topic"]            = base + "/set";
    o["brightness_state_topic"]   = base + "/brightness";
    o["brightness_command_topic"] = base + "/brightness/set";
    o["brightness_scale"]         = 255;
    o["payload_on"]               = "ON";
    o["payload_off"]              = "OFF";
    o["retain"]                   = true;
    o["device"]                   = device_info(s);
    return json::serialize(o);
}

std::string make_number_discovery(const std::string& p, const std::string& s,
                                    const config::PcaConfig& e) {
    const std::string uid  = std::format("myio_{}_pca_{}", s, e.id);
    const std::string base = std::format("{}/{}/number/{}", p, s, e.id);
    json::object o;
    o["name"]                = e.name.empty() ? uid : e.name;
    o["unique_id"]           = uid;
    o["state_topic"]         = base + "/state";
    o["command_topic"]       = base + "/set";
    o["min"]                 = 0;
    o["max"]                 = 100;
    o["step"]                = 1;
    o["unit_of_measurement"] = "%";
    o["retain"]              = true;
    o["device"]              = device_info(s);
    return json::serialize(o);
}

std::string make_number_discovery(const std::string& p, const std::string& s,
                                    const config::PwmConfig& e) {
    const std::string uid  = std::format("myio_{}_pwm_{}", s, e.id);
    const std::string base = std::format("{}/{}/number/{}", p, s, e.id);
    json::object o;
    o["name"]                = e.name.empty() ? uid : e.name;
    o["unique_id"]           = uid;
    o["state_topic"]         = base + "/state";
    o["command_topic"]       = base + "/set";
    o["min"]                 = 0;
    o["max"]                 = 100;
    o["step"]                = 1;
    o["unit_of_measurement"] = "%";
    o["retain"]              = true;
    o["device"]              = device_info(s);
    return json::serialize(o);
}

std::string make_cover_discovery(const std::string& p, const std::string& s,
                                  const config::CoverConfig& c) {
    const std::string uid  = std::format("myio_{}_cover_{}", s, c.cover_id);
    const std::string base = std::format("{}/{}/cover/{}", p, s, c.cover_id);
    json::object o;
    o["name"]          = c.name.empty() ? uid : c.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    o["payload_open"]  = "OPEN";
    o["payload_close"] = "CLOSE";
    o["payload_stop"]  = "STOP";
    o["state_topic"]   = std::format("{}/{}/cover/{}/state",    p, s, c.cover_id);
    o["position_topic"]= std::format("{}/{}/cover/{}/position", p, s, c.cover_id);
    o["state_open"]    = "open";
    o["state_closed"]  = "closed";
    o["state_opening"] = "opening";
    o["state_closing"] = "closing";
    o["state_stopped"] = "stopped";
    o["retain"]        = true;
    o["device"]        = device_info(s);
    return json::serialize(o);
}

std::string make_sensor_discovery(const std::string& p, const std::string& s,
                                   const config::SensorConfig& sen) {
    const std::string uid = std::format("myio_{}_sensor_{}", s, sen.id);
    json::object o;
    o["name"]        = sen.name.empty() ? uid : sen.name;
    o["unique_id"]   = uid;
    o["state_topic"] = sensor_state_topic(p, s, sen.id);
    o["retain"]      = true;
    if (sen.id >= 1   && sen.id <= 100) o["unit_of_measurement"] = "°C";
    if (sen.id >= 101 && sen.id <= 108) o["unit_of_measurement"] = "%";
    if (sen.id >= 201 && sen.id <= 203) o["unit_of_measurement"] = "kW";
    if (sen.id >= 204 && sen.id <= 206) o["unit_of_measurement"] = "V";
    if (sen.id >= 207 && sen.id <= 209) o["unit_of_measurement"] = "A";
    o["device"] = device_info(s);
    return json::serialize(o);
}

// ── Discovery topic helpers ───────────────────────────────────────────────────
// Returns the homeassistant/... discovery topic for a given entity.
// Publishing an empty string to this topic removes the entity from HA.

static std::string pca_discovery_topic(const std::string& s, const config::PcaConfig& e) {
    std::string type;
    switch (e.type) {
        case config::OutputType::Light:  type = "light";  break;
        case config::OutputType::Number: type = "number"; break;
        default:                         type = "switch"; break;
    }
    return std::format("homeassistant/{}/myio_{}_{}/config", type, s, e.id);
}

static std::string relay_discovery_topic(const std::string& s, const config::RelayConfig& e) {
    return std::format("homeassistant/switch/myio_{}_{}/config", s, e.id);
}

static std::string pwm_discovery_topic(const std::string& s, const config::PwmConfig& e) {
    const std::string type = (e.type == config::OutputType::Number) ? "number" : "light";
    return std::format("homeassistant/{}/myio_{}_{}/config", type, s, e.id);
}

static std::string sensor_discovery_topic(const std::string& s, const config::SensorConfig& e) {
    return std::format("homeassistant/sensor/myio_{}_{}/config", s, e.id);
}

static std::string cover_discovery_topic(const std::string& s, const config::CoverConfig& c) {
    return std::format("homeassistant/cover/myio_{}_{}/config", s, c.cover_id);
}

// ── Utility ───────────────────────────────────────────────────────────────────

static std::string normalize_payload(std::string s) {
    std::ranges::transform(s, s.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

static std::vector<std::string> split_topic(const std::string& topic) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : topic) {
        if (c == '/') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

// ── ServerBridge ──────────────────────────────────────────────────────────────

ServerBridge::ServerBridge(config::ServerConfig               cfg,
                            std::string                        topic_prefix,
                            std::shared_ptr<ha::IHAPublisher>  publisher,
                            std::unique_ptr<myio::IMyIOClient> client)
    : cfg_(std::move(cfg))
    , topic_prefix_(std::move(topic_prefix))
    , publisher_(std::move(publisher))
    , client_(std::move(client))
{}

std::unordered_set<int> ServerBridge::cover_output_ids() const {
    std::unordered_set<int> ids;
    for (const auto& c : cfg_.covers) {
        ids.insert(c.up.id);
        ids.insert(c.down.id);
    }
    return ids;
}

const config::CoverConfig* ServerBridge::find_cover_by_output(int output_id) const {
    for (const auto& c : cfg_.covers)
        if (c.up.id == output_id || c.down.id == output_id)
            return &c;
    return nullptr;
}

// ── Discovery ─────────────────────────────────────────────────────────────────
// For enabled entities:  publish the discovery JSON (HA registers them).
// For disabled entities: publish an empty retained message (HA removes them).
// Cover endpoint types (cover_up/cover_down) are never published as standalone
// entities regardless of enabled flag.

net::awaitable<void> ServerBridge::publish_discovery() {
    const auto& p = topic_prefix_;
    const auto& s = cfg_.server_id;

    // Fetch initial state to determine PCA hardware capabilities (pwm flag).
    // pca_is_pwm_ is a one-time cache — the pwm flag is a hardware property that never changes.
    if (pca_is_pwm_.empty()) {
        try {
            const auto state = co_await client_->async_poll();
            for (const auto& pca : state.pca_outputs)
                pca_is_pwm_[pca.id] = pca.is_pwm;
        } catch (const std::exception& ex) {
            myio::log(stderr, "[{}] Could not determine PCA capabilities at discovery: {}",
                      s, myio::short_what(ex));
        }
    }

    for (const auto& e : cfg_.pca) {
        // Skip cover endpoints – they are only represented as cover entities
        if (e.type == config::OutputType::CoverUp ||
            e.type == config::OutputType::CoverDown) continue;

        const std::string dtopic = pca_discovery_topic(s, e);

        // Clear stale discovery topics from previous type assignments.
        // If this PCA was previously a switch and is now a light, HA keeps both
        // until the old topic is explicitly cleared with an empty retained message.
        for (const char* t : {"switch", "light", "number"}) {
            const std::string stale = std::format("homeassistant/{}/myio_{}_{}/config", t, s, e.id);
            if (stale != dtopic)
                co_await publisher_->async_publish(stale, "", true);
        }

        if (e.enabled) {
            std::string payload;
            switch (e.type) {
                case config::OutputType::Light: {
                    const bool can_dim = pca_is_pwm_.count(e.id) && pca_is_pwm_.at(e.id);
                    if (!can_dim)
                        myio::log(stderr, "[{}] PCA {} configured as light but pwm=0 – "
                                         "publishing without brightness control", s, e.id);
                    payload = make_light_discovery(p, s, e, can_dim);
                    break;
                }
                case config::OutputType::Number:
                    payload = make_number_discovery(p, s, e); break;
                default:
                    payload = make_switch_discovery(p, s, e); break;
            }
            co_await publisher_->async_publish(dtopic, payload, true);
        } else {
            // Undiscovery: empty retained message removes entity from HA
            co_await publisher_->async_publish(dtopic, "", true);
        }
    }

    for (const auto& e : cfg_.relays) {
        if (e.type == config::OutputType::CoverUp ||
            e.type == config::OutputType::CoverDown) continue;

        const std::string dtopic = relay_discovery_topic(s, e);
        if (e.enabled)
            co_await publisher_->async_publish(dtopic, make_switch_discovery(p, s, e), true);
        else
            co_await publisher_->async_publish(dtopic, "", true);
    }

    for (const auto& e : cfg_.pwm) {
        const std::string dtopic = pwm_discovery_topic(s, e);
        if (e.enabled) {
            const std::string payload = (e.type == config::OutputType::Number)
                ? make_number_discovery(p, s, e)
                : make_light_discovery(p, s, e);
            co_await publisher_->async_publish(dtopic, payload, true);
        } else {
            co_await publisher_->async_publish(dtopic, "", true);
        }
    }

    for (const auto& e : cfg_.sensors) {
        const std::string dtopic = sensor_discovery_topic(s, e);
        if (e.enabled)
            co_await publisher_->async_publish(dtopic, make_sensor_discovery(p, s, e), true);
        else
            co_await publisher_->async_publish(dtopic, "", true);
    }

    for (const auto& c : cfg_.covers) {
        const std::string dtopic = cover_discovery_topic(s, c);
        if (c.enabled) {
            co_await publisher_->async_publish(dtopic, make_cover_discovery(p, s, c), true);
            // Reset any stale retained state (e.g. "open"/"closed" from a previous run)
            co_await publisher_->async_publish(cover_state_topic(p, s, c.cover_id), "stopped", true);
            co_await publisher_->async_publish(cover_position_topic(p, s, c.cover_id), "50", true);
        } else {
            co_await publisher_->async_publish(dtopic, "", true);
        }
    }

    myio::log("[{}] Discovery published.", cfg_.server_id);
}

// ── State publishers ──────────────────────────────────────────────────────────

net::awaitable<void> ServerBridge::publish_pca_state(const myio::PcaOutput& pca,
                                                       const config::PcaConfig& cfg) {
    if (cfg.type == config::OutputType::Number) {
        const int pct = static_cast<int>(std::round(pca.state / 255.0 * 100.0));
        co_await publisher_->async_publish(
            number_state_topic(topic_prefix_, cfg_.server_id, pca.id),
            std::to_string(pct), true);
        myio::log("[{}] [myIO to broker] PCA {} → {}%", cfg_.server_id, pca.id, pct);
        co_return;
    }

    const std::string on_off = (pca.state > pca.turn_off) ? "ON" : "OFF";
    if (cfg.type == config::OutputType::Light) {
        co_await publisher_->async_publish(
            light_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
        if (pca.is_pwm) {
            const int pct = static_cast<int>(std::round(pca.state / 255.0 * 100.0));
            co_await publisher_->async_publish(
                light_bright_topic(topic_prefix_, cfg_.server_id, pca.id),
                std::to_string(pca.state), true);
            myio::log("[{}] [myIO to broker] PCA {} → {} brightness={} ({}%)",
                      cfg_.server_id, pca.id, on_off, pca.state, pct);
        } else {
            myio::log("[{}] [myIO to broker] PCA {} → {}", cfg_.server_id, pca.id, on_off);
        }
    } else {
        co_await publisher_->async_publish(
            switch_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
        myio::log("[{}] [myIO to broker] PCA {} → {}", cfg_.server_id, pca.id, on_off);
    }
}

net::awaitable<void> ServerBridge::publish_relay_state(const myio::RelayOutput& relay,
                                                         const config::RelayConfig& /*cfg*/) {
    const char* on_off = relay.state ? "ON" : "OFF";
    co_await publisher_->async_publish(
        switch_state_topic(topic_prefix_, cfg_.server_id, relay.id), on_off, true);
    myio::log("[{}] [myIO to broker] Relay {} → {}", cfg_.server_id, relay.id, on_off);
}

net::awaitable<void> ServerBridge::publish_cover_state(const config::CoverConfig& cover,
                                                         int up_state, int down_state, int dir_hint) {
    std::string state;
    if      (up_state > 0 && down_state == 0) state = "opening";
    else if (down_state > 0 && up_state == 0) state = "closing";
    else                                       state = "stopped";
    co_await publisher_->async_publish(
        cover_state_topic(topic_prefix_, cfg_.server_id, cover.cover_id), state, true);
    // Publish position when stopped so HA knows the cover is not at an end stop.
    // Without position, HA auto-transitions "stopped" to "open" or "closed",
    // which grays out one direction button.
    if (state == "stopped") {
        const int pos = (dir_hint > 0) ? 100 : (dir_hint < 0) ? 0 : 50;
        co_await publisher_->async_publish(
            cover_position_topic(topic_prefix_, cfg_.server_id, cover.cover_id),
            std::to_string(pos), true);
    }
}

net::awaitable<void> ServerBridge::publish_pwm_state(const myio::PwmOutput& pwm,
                                                       const config::PwmConfig& cfg) {
    if (cfg.type == config::OutputType::Number) {
        const int pct = static_cast<int>(std::round(pwm.state / 255.0 * 100.0));
        co_await publisher_->async_publish(
            number_state_topic(topic_prefix_, cfg_.server_id, pwm.id),
            std::to_string(pct), true);
        myio::log("[{}] [myIO to broker] PWM {} → {}%", cfg_.server_id, pwm.id, pct);
        co_return;
    }

    const std::string on_off = (pwm.state > pwm.turn_off) ? "ON" : "OFF";
    const int pct = static_cast<int>(std::round(pwm.state / 255.0 * 100.0));
    co_await publisher_->async_publish(
        light_state_topic(topic_prefix_, cfg_.server_id, pwm.id), on_off, true);
    co_await publisher_->async_publish(
        light_bright_topic(topic_prefix_, cfg_.server_id, pwm.id),
        std::to_string(pwm.state), true);
    myio::log("[{}] [myIO to broker] PWM {} → {} brightness={} ({}%)",
              cfg_.server_id, pwm.id, on_off, pwm.state, pct);
}

net::awaitable<void> ServerBridge::publish_sensor_state(const myio::Sensor& sensor,
                                                          const config::SensorConfig& /*cfg*/) {
    std::string val;
    if      (sensor.temperature) val = std::format("{:.2f}", *sensor.temperature);
    else if (sensor.humidity)    val = std::format("{:.2f}", *sensor.humidity);
    else if (sensor.power)       val = std::format("{:.2f}", *sensor.power);
    else if (sensor.voltage)     val = std::format("{:.2f}", *sensor.voltage);
    else if (sensor.current)     val = std::format("{:.2f}", *sensor.current);
    else if (sensor.energy)      val = std::format("{:.2f}", *sensor.energy);
    else co_return;
    co_await publisher_->async_publish(
        sensor_state_topic(topic_prefix_, cfg_.server_id, sensor.id), val, true);
}

// ── Poll loop ─────────────────────────────────────────────────────────────────

net::awaitable<void> ServerBridge::poll_loop() {
    auto ex = co_await net::this_coro::executor;
    const auto cover_ids = cover_output_ids();

    // Build fast id → config lookup maps (enabled entries only for state publishing)
    std::unordered_map<int, const config::PcaConfig*>    pca_map;
    std::unordered_map<int, const config::RelayConfig*>  relay_map;
    std::unordered_map<int, const config::SensorConfig*> sensor_map;
    std::unordered_map<int, const config::PwmConfig*>    pwm_map;
    for (const auto& e : cfg_.pca)
        if (e.enabled) pca_map[e.id] = &e;
    for (const auto& e : cfg_.relays)
        if (e.enabled) relay_map[e.id] = &e;
    for (const auto& e : cfg_.sensors)
        if (e.enabled) sensor_map[e.id] = &e;
    for (const auto& e : cfg_.pwm)
        if (e.enabled) pwm_map[e.id] = &e;

    std::unordered_map<int, const config::CoverConfig*> cover_map;
    for (const auto& c : cfg_.covers)
        if (c.enabled) {
            cover_map[c.up.id]   = &c;
            cover_map[c.down.id] = &c;
        }

    try {
        const auto info = co_await client_->async_get_info();
        myio::log("[{}] myIO v{} | {} | serial {} | {} relays, {} PCA | API v{}",
                  cfg_.server_id, info.version, info.cpu, info.serial,
                  info.relay_count, info.pca_count, info.api);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[{}] Server info unavailable: {}", cfg_.server_id, myio::short_what(ex));
    }

    myio::log("[{}] Poll loop started, interval={}s",
              cfg_.server_id, cfg_.poll_interval_sec);

    // Sentinel init: force first poll to publish current state regardless of value.
    // Without this, state=0 (OFF) would match the default-initialized last=0 → no publish.
    for (const auto& [id, _] : pca_map)    last_pca_state_[id]   = -1;
    for (const auto& [id, _] : relay_map)  last_relay_state_[id] = -1;
    for (const auto& [id, _] : pwm_map)    last_pwm_state_[id]   = -1;
    for (const auto& [id, _] : sensor_map) last_sensor_val_[id]  = std::numeric_limits<double>::max();

    while (true) {
        std::exception_ptr caught;
        try {
            const auto state = co_await client_->async_poll();

            for (const auto& pca : state.pca_outputs) {
                if (cover_ids.contains(pca.id)) {
                    const auto* cover = cover_map.count(pca.id) ? cover_map.at(pca.id) : nullptr;
                    if (!cover) continue;

                    const int other_id = (pca.id == cover->up.id)
                                           ? cover->down.id : cover->up.id;
                    int other_state = 0;
                    for (const auto& o : state.pca_outputs)
                        if (o.id == other_id) { other_state = o.state; break; }

                    const int up_state   = (pca.id == cover->up.id)   ? pca.state : other_state;
                    const int down_state = (pca.id == cover->down.id) ? pca.state : other_state;
                    const int combined   = up_state * 1000 + down_state;
                    const int key        = -(cover->up.id);

                    auto& last = last_pca_state_[key];
                    if (last != combined) {
                        if      (up_state > 0)   last_cover_dir_[key] = +1;
                        else if (down_state > 0) last_cover_dir_[key] = -1;
                        last = combined;
                        const int dir = last_cover_dir_.count(key) ? last_cover_dir_.at(key) : 0;
                        co_await publish_cover_state(*cover, up_state, down_state, dir);
                    }
                    continue;
                }

                const auto it = pca_map.find(pca.id);
                if (it == pca_map.end()) continue;

                auto& last = last_pca_state_[pca.id];
                if (last != pca.state) {
                    last = pca.state;
                    co_await publish_pca_state(pca, *it->second);
                }
            }

            for (const auto& relay : state.relay_outputs) {
                if (cover_ids.contains(relay.id)) {
                    const auto* cover = cover_map.count(relay.id) ? cover_map.at(relay.id) : nullptr;
                    if (!cover) continue;

                    const int other_id = (relay.id == cover->up.id)
                                           ? cover->down.id : cover->up.id;
                    int other_state = 0;
                    for (const auto& o : state.relay_outputs)
                        if (o.id == other_id) { other_state = o.state ? 1 : 0; break; }

                    const int up_state   = (relay.id == cover->up.id)   ? (relay.state ? 1 : 0) : other_state;
                    const int down_state = (relay.id == cover->down.id) ? (relay.state ? 1 : 0) : other_state;
                    const int combined   = up_state * 1000 + down_state;
                    const int key        = -(cover->up.id);

                    auto& last = last_relay_state_[key];
                    if (last != combined) {
                        if      (up_state > 0)   last_cover_dir_[key] = +1;
                        else if (down_state > 0) last_cover_dir_[key] = -1;
                        last = combined;
                        const int dir = last_cover_dir_.count(key) ? last_cover_dir_.at(key) : 0;
                        co_await publish_cover_state(*cover, up_state, down_state, dir);
                    }
                    continue;
                }

                const auto it = relay_map.find(relay.id);
                if (it == relay_map.end()) continue;
                auto& last = last_relay_state_[relay.id];
                if (last != static_cast<int>(relay.state)) {
                    last = relay.state ? 1 : 0;
                    co_await publish_relay_state(relay, *it->second);
                }
            }

            for (const auto& pwm : state.pwm_outputs) {
                const auto it = pwm_map.find(pwm.id);
                if (it == pwm_map.end()) continue;
                auto& last = last_pwm_state_[pwm.id];
                if (last != pwm.state) {
                    last = pwm.state;
                    co_await publish_pwm_state(pwm, *it->second);
                }
            }

            for (const auto& sensor : state.sensors) {
                const auto it = sensor_map.find(sensor.id);
                if (it == sensor_map.end()) continue;
                double val = 0.0;
                if      (sensor.temperature) val = *sensor.temperature;
                else if (sensor.humidity)    val = *sensor.humidity;
                else if (sensor.power)       val = *sensor.power;
                else continue;
                auto& last = last_sensor_val_[sensor.id];
                if (std::abs(last - val) > 0.005) {
                    last = val;
                    co_await publish_sensor_state(sensor, *it->second);
                }
            }

        } catch (...) {
            caught = std::current_exception();
        }
        if (caught) {
            try { std::rethrow_exception(caught); }
            catch (const std::exception& ex) {
                std::string msg = myio::short_what(ex);
                if (msg == last_poll_error_) {
                    myio::log(stderr, "[{}] Poll error: (same as previous)", cfg_.server_id);
                } else {
                    last_poll_error_ = msg;
                    myio::log(stderr, "[{}] Poll error: {}", cfg_.server_id, msg);
                }
            }
        } else {
            last_poll_error_.clear();
        }

        net::steady_timer timer(ex);
        timer.expires_after(std::chrono::seconds(cfg_.poll_interval_sec));
        co_await timer.async_wait(net::use_awaitable);
    }
}

// ── Command handler ───────────────────────────────────────────────────────────
// Only enabled entities receive commands. Disabled entities are silently ignored.

net::awaitable<void> ServerBridge::handle_command(const std::string& topic,
                                                    const std::string& payload) {
    const auto parts = split_topic(topic);
    if (parts.size() < 5) co_return;
    if (parts[1] != cfg_.server_id) co_return;

    const std::string& entity_type = parts[2];
    const std::string& id_str      = parts[3];
    const std::string  cmd         = normalize_payload(payload);

    // Cover command
    if (entity_type == "cover") {
        if (parts[4] != "set") co_return;
        for (const auto& c : cfg_.covers) {
            if (!c.enabled || c.cover_id != id_str) continue;
            auto ep_on = [](const config::CoverEndpoint& ep) -> std::string {
                return ep.output_type == "relay"
                    ? std::format("r_ON={}",   ep.id)
                    : std::format("PCA_ON={}", ep.id);
            };
            auto ep_off = [](const config::CoverEndpoint& ep) -> std::string {
                return ep.output_type == "relay"
                    ? std::format("r_OFF={}",   ep.id)
                    : std::format("PCA_OFF={}", ep.id);
            };
            std::string post;
            if      (cmd == "OPEN")  post = ep_off(c.down) + "&" + ep_on(c.up);
            else if (cmd == "CLOSE") post = ep_off(c.up)   + "&" + ep_on(c.down);
            else if (cmd == "STOP")  post = ep_off(c.up)   + "&" + ep_off(c.down);
            else co_return;
            myio::log("[{}] [broker to myIO] Cover {} → {}", cfg_.server_id, c.cover_id, post);
            co_await client_->async_send_command(post);

            // Optimistic state publish + direction tracking.
            // The poll interval (30 s) is longer than most motor runs, so direction
            // and state must be set from the command, not from polling.
            const int key = -(c.up.id);
            if (cmd == "OPEN") {
                last_cover_dir_[key] = +1;
                if (c.up.output_type == "relay") last_relay_state_[key] = 1;
                else                             last_pca_state_[key]   = 1;
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "opening", true);
                myio::log("[{}] [bridge to broker] Cover {} → opening", cfg_.server_id, c.cover_id);
            } else if (cmd == "CLOSE") {
                last_cover_dir_[key] = -1;
                if (c.up.output_type == "relay") last_relay_state_[key] = 1;
                else                             last_pca_state_[key]   = 1;
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "closing", true);
                myio::log("[{}] [bridge to broker] Cover {} → closing", cfg_.server_id, c.cover_id);
            } else if (cmd == "STOP") {
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "stopped", true);
                co_await publisher_->async_publish(
                    cover_position_topic(topic_prefix_, cfg_.server_id, c.cover_id), "50", true);
                myio::log("[{}] [bridge to broker] Cover {} → stopped", cfg_.server_id, c.cover_id);
                if (last_cover_cancel_.count(key))
                    *last_cover_cancel_[key] = true;
            }

            // After OPEN/CLOSE, the motor stops on its own when it reaches the end stop.
            // The poll may not catch this within the 30 s interval, leaving HA stuck in
            // "opening"/"closing". Spawn a timer that publishes "stopped" after 90 s as
            // a safety net. If the user presses STOP first, or the poll catches it sooner,
            // this publish is a harmless duplicate.
            if (cmd == "OPEN" || cmd == "CLOSE") {
                if (c.motor_timeout_sec > 0) {
                    const int end_pos = (cmd == "OPEN") ? 100 : 0;
                    auto flag = std::make_shared<bool>(false);
                    last_cover_cancel_[key] = flag;
                    net::co_spawn(co_await net::this_coro::executor,
                        [pub = publisher_, prefix = topic_prefix_,
                         sid = cfg_.server_id, cov_id = c.cover_id,
                         end_pos, timeout = c.motor_timeout_sec, flag]
                        () -> net::awaitable<void> {
                            try {
                                net::steady_timer t(co_await net::this_coro::executor);
                                t.expires_after(std::chrono::seconds(timeout));
                                co_await t.async_wait(net::use_awaitable);
                                if (*flag) co_return;  // STOP was pressed, skip end-position publish
                                co_await pub->async_publish(
                                    cover_state_topic(prefix, sid, cov_id), "stopped", true);
                                co_await pub->async_publish(
                                    cover_position_topic(prefix, sid, cov_id), std::to_string(end_pos), true);
                                myio::log("[{}] [bridge to broker] Cover {} → stopped (motor timeout)", sid, cov_id);
                            } catch (const std::exception& ex) {
                                myio::log(stderr, "[{}] Cover {} motor timer error: {}",
                                          sid, cov_id, myio::short_what(ex));
                            } catch (...) {}
                        }(), net::detached);
                }
            }
            co_return;
        }
        co_return;
    }

    int id = 0;
    if (std::from_chars(id_str.data(), id_str.data() + id_str.size(), id).ec != std::errc{})
        co_return;

    // Number value (0–100, passed through to PCA*/fet* as percentage)
    if (entity_type == "number" && parts[4] == "set") {
        const bool is_pca = std::ranges::any_of(cfg_.pca,
            [id](const auto& e) {
                return e.id == id && e.enabled && e.type == config::OutputType::Number;
            });
        const bool is_pwm = std::ranges::any_of(cfg_.pwm,
            [id](const auto& e) {
                return e.id == id && e.enabled && e.type == config::OutputType::Number;
            });
        if (!is_pca && !is_pwm) co_return;

        int value = 0;
        std::from_chars(payload.data(), payload.data() + payload.size(), value);
        value = std::clamp(value, 0, 100);

        const std::string post = is_pca
            ? std::format("PCA*{}={}", id,       value)
            : std::format("fet*{}={}", id - 100, value);
        myio::log("[{}] [broker to myIO] Number {} → {}", cfg_.server_id, id, post);
        co_await client_->async_send_command(post);
        co_return;
    }

    // Light brightness
    if (entity_type == "light" && parts.size() >= 6 && parts[4] == "brightness" && parts[5] == "set") {
        const bool is_pca_light = std::ranges::any_of(cfg_.pca,
            [id, this](const auto& e) {
                return e.id == id && e.enabled && e.type == config::OutputType::Light
                    && pca_is_pwm_.count(id) && pca_is_pwm_.at(id);
            });
        const bool is_pwm_light = std::ranges::any_of(cfg_.pwm,
            [id](const auto& e) {
                return e.id == id && e.enabled && e.type != config::OutputType::Number;
            });
        if (!is_pca_light && !is_pwm_light) co_return;

        int brightness = 0;
        std::from_chars(payload.data(), payload.data() + payload.size(), brightness);
        const std::string post = is_pca_light
            ? std::format("PCA*{}={}", id, brightness)
            : std::format("fet*{}={}", id - 100, brightness);
        myio::log("[{}] [broker to myIO] Light brightness {} → {}", cfg_.server_id, id, post);
        co_await client_->async_send_command(post);

        // Optimistic state publish — like ON/OFF, don't wait for next poll
        const std::string on_off = (brightness > 0) ? "ON" : "OFF";
        co_await publisher_->async_publish(
            light_state_topic(topic_prefix_, cfg_.server_id, id), on_off, true);
        co_await publisher_->async_publish(
            light_bright_topic(topic_prefix_, cfg_.server_id, id),
            std::to_string(brightness), true);
        if (is_pca_light) last_pca_state_[id] = brightness;
        else              last_pwm_state_[id]  = brightness;
        myio::log("[{}] [bridge to broker] Light brightness {} → {}", cfg_.server_id, id, brightness);
        co_return;
    }

    // Switch / light ON-OFF
    if ((entity_type == "switch" || entity_type == "light") && parts[4] == "set") {
        // Find enabled matching entry (Number outputs are excluded – they have their own handler above)
        const bool is_pca = std::ranges::any_of(cfg_.pca,
            [id](const auto& e) {
                return e.id == id && e.enabled && e.type != config::OutputType::Number;
            });
        const bool is_relay = std::ranges::any_of(cfg_.relays,
            [id](const auto& e) { return e.id == id && e.enabled; });
        const bool is_pwm = std::ranges::any_of(cfg_.pwm,
            [id](const auto& e) {
                return e.id == id && e.enabled && e.type != config::OutputType::Number;
            });

        std::string post;
        if (is_pca) {
            post = (cmd == "ON") ? std::format("PCA_ON={}", id) : std::format("PCA_OFF={}", id);
        } else if (is_pwm) {
            post = (cmd == "ON") ? std::format("f_ON={}", id - 100) : std::format("f_OFF={}", id - 100);
        } else if (is_relay) {
            post = (cmd == "ON") ? std::format("r_ON={}", id) : std::format("r_OFF={}", id);
        } else {
            co_return;
        }

        myio::log("[{}] [broker to myIO] {} {} → {}", cfg_.server_id, entity_type, id, post);
        co_await client_->async_send_command(post);

        // Publish optimistic state immediately so HA doesn't revert to the
        // old retained state while waiting for the next poll cycle.
        if (is_pca) {
            const auto it = std::ranges::find_if(cfg_.pca,
                [id](const auto& e) { return e.id == id && e.enabled && e.type != config::OutputType::Number; });
            if (it != cfg_.pca.end()) {
                const std::string stopic = (it->type == config::OutputType::Light)
                    ? light_state_topic(topic_prefix_, cfg_.server_id, id)
                    : switch_state_topic(topic_prefix_, cfg_.server_id, id);
                co_await publisher_->async_publish(stopic, cmd, true);
                myio::log("[{}] [bridge to broker] {} {} → {}", cfg_.server_id, entity_type, id, cmd);
            }
        } else if (is_relay) {
            co_await publisher_->async_publish(
                switch_state_topic(topic_prefix_, cfg_.server_id, id), cmd, true);
            myio::log("[{}] [bridge to broker] {} {} → {}", cfg_.server_id, entity_type, id, cmd);
        } else if (is_pwm) {
            co_await publisher_->async_publish(
                light_state_topic(topic_prefix_, cfg_.server_id, id), cmd, true);
            myio::log("[{}] [bridge to broker] {} {} → {}", cfg_.server_id, entity_type, id, cmd);
        }
    }
}

// ── Bridge ────────────────────────────────────────────────────────────────────

Bridge::Bridge(config::BridgeConfig              cfg,
               net::io_context&                  ioc,
               std::shared_ptr<ha::IHAPublisher> publisher)
    : cfg_(std::move(cfg))
    , ioc_(ioc)
    , publisher_(std::move(publisher))
{
    for (auto& srv : cfg_.servers) {
        myio::HttpConfig hcfg;
        hcfg.host      = srv.host;
        hcfg.port      = srv.port;
        hcfg.user      = srv.user;
        hcfg.password  = srv.password;
        hcfg.server_id = srv.server_id;

        servers_.push_back(std::make_unique<ServerBridge>(
            srv,
            cfg_.mqtt.topic_prefix,
            publisher_,
            std::make_unique<myio::AsyncHttpClient>(hcfg)));
    }
}

net::awaitable<void> Bridge::run() {
    co_await publisher_->async_connect();
    co_await publisher_->async_subscribe(cfg_.mqtt.topic_prefix + "/#");
    co_await publisher_->async_subscribe("homeassistant/status");

    for (auto& srv : servers_)
        co_await srv->publish_discovery();

    for (auto& srv : servers_) {
        net::co_spawn(ioc_, srv->poll_loop(),
            [sid = srv->server_id()](std::exception_ptr ep) {
                if (ep) {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& ex) {
                        myio::log(stderr, "[{}] Poll coroutine crashed: {}",
                                  sid, myio::short_what(ex));
                    }
                }
            });
    }

    co_await command_loop();
}

net::awaitable<void> Bridge::command_loop() {
    myio::log("[bridge] Command loop started.");
    while (true) {
        std::exception_ptr caught;
        try {
            auto [topic, payload] = co_await publisher_->async_receive();

            if (topic == "homeassistant/status" && payload == "online") {
                myio::log("[bridge] HA restarted – republishing discovery.");
                for (auto& srv : servers_)
                    co_await srv->publish_discovery();
                continue;
            }

            for (auto& srv : servers_)
                co_await srv->handle_command(topic, payload);
        } catch (...) {
            caught = std::current_exception();
        }
        if (caught) {
            try { std::rethrow_exception(caught); }
            catch (const std::exception& ex) {
                myio::log(stderr, "[bridge] Command loop error: {}", myio::short_what(ex));
            }
            net::steady_timer t(co_await net::this_coro::executor);
            t.expires_after(30s);
            co_await t.async_wait(net::use_awaitable);
            try {
                co_await publisher_->async_subscribe(cfg_.mqtt.topic_prefix + "/#");
                co_await publisher_->async_subscribe("homeassistant/status");
                myio::log("[bridge] Reconnected – republishing discovery.");
                for (auto& srv : servers_)
                    co_await srv->publish_discovery();
            } catch (const std::exception& ex) {
                myio::log(stderr, "[bridge] Reconnect setup error: {}", myio::short_what(ex));
            }
        }
    }
}

} // namespace bridge
