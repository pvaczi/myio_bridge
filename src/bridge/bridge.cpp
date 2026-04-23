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
std::string cover_cmd_topic(const std::string& p, const std::string& s,
                             const std::string& cid) {
    return std::format("{}/{}/cover/{}/set", p, s, cid);
}
std::string sensor_state_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/sensor/{}/state", p, s, id);
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
                                  const config::PcaConfig& e) {
    const std::string uid  = std::format("myio_{}_pca_{}", s, e.id);
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
    const std::string type = (e.type == config::OutputType::Light) ? "light" : "switch";
    return std::format("homeassistant/{}/myio_{}_{}/config", type, s, e.id);
}

static std::string relay_discovery_topic(const std::string& s, const config::RelayConfig& e) {
    return std::format("homeassistant/switch/myio_{}_{}/config", s, e.id);
}

static std::string pwm_discovery_topic(const std::string& s, const config::PwmConfig& e) {
    return std::format("homeassistant/light/myio_{}_{}/config", s, e.id);
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

    for (const auto& e : cfg_.pca) {
        // Skip cover endpoints – they are only represented as cover entities
        if (e.type == config::OutputType::CoverUp ||
            e.type == config::OutputType::CoverDown) continue;

        const std::string dtopic = pca_discovery_topic(s, e);
        if (e.enabled) {
            const std::string payload = (e.type == config::OutputType::Light)
                ? make_light_discovery(p, s, e)
                : make_switch_discovery(p, s, e);
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
        if (e.enabled)
            co_await publisher_->async_publish(dtopic, make_light_discovery(p, s, e), true);
        else
            co_await publisher_->async_publish(dtopic, "", true);
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
        if (c.enabled)
            co_await publisher_->async_publish(dtopic, make_cover_discovery(p, s, c), true);
        else
            co_await publisher_->async_publish(dtopic, "", true);
    }

    myio::log("[{}] Discovery published.", cfg_.server_id);
}

// ── State publishers ──────────────────────────────────────────────────────────

net::awaitable<void> ServerBridge::publish_pca_state(const myio::PcaOutput& pca,
                                                       const config::PcaConfig& cfg) {
    const std::string on_off = (pca.state > pca.turn_off) ? "ON" : "OFF";
    if (cfg.type == config::OutputType::Light) {
        co_await publisher_->async_publish(
            light_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
        co_await publisher_->async_publish(
            light_bright_topic(topic_prefix_, cfg_.server_id, pca.id),
            std::to_string(pca.state), true);
    } else {
        co_await publisher_->async_publish(
            switch_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
    }
}

net::awaitable<void> ServerBridge::publish_relay_state(const myio::RelayOutput& relay,
                                                         const config::RelayConfig& /*cfg*/) {
    co_await publisher_->async_publish(
        switch_state_topic(topic_prefix_, cfg_.server_id, relay.id),
        relay.state ? "ON" : "OFF", true);
}

net::awaitable<void> ServerBridge::publish_cover_state(const config::CoverConfig& cover,
                                                         int up_state, int down_state) {
    std::string state;
    if      (up_state > 0 && down_state == 0) state = "opening";
    else if (down_state > 0 && up_state == 0) state = "closing";
    else                                       state = "stopped";
    co_await publisher_->async_publish(
        cover_state_topic(topic_prefix_, cfg_.server_id, cover.cover_id), state, true);
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
    for (const auto& e : cfg_.pca)
        if (e.enabled) pca_map[e.id] = &e;
    for (const auto& e : cfg_.relays)
        if (e.enabled) relay_map[e.id] = &e;
    for (const auto& e : cfg_.sensors)
        if (e.enabled) sensor_map[e.id] = &e;

    std::unordered_map<int, const config::CoverConfig*> cover_map;
    for (const auto& c : cfg_.covers)
        if (c.enabled) {
            cover_map[c.up.id]   = &c;
            cover_map[c.down.id] = &c;
        }

    myio::log("[{}] Poll loop started, interval={}s",
              cfg_.server_id, cfg_.poll_interval_sec);

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

                    auto& last = last_pca_state_[-(cover->up.id)];
                    if (last != combined) {
                        last = combined;
                        co_await publish_cover_state(*cover, up_state, down_state);
                    }
                    continue;
                }

                const auto it = pca_map.find(pca.id);
                if (it == pca_map.end()) continue;

                auto& last = last_pca_state_[pca.id];
                if (last != pca.state) {
                    last = pca.state;
                    co_await publish_pca_state(pca, *it->second);
                    myio::log("[{}] PCA {} → {}", cfg_.server_id, pca.id, pca.state);
                }
            }

            for (const auto& relay : state.relay_outputs) {
                const auto it = relay_map.find(relay.id);
                if (it == relay_map.end()) continue;
                auto& last = last_relay_state_[relay.id];
                if (last != relay.state) {
                    last = relay.state;
                    co_await publish_relay_state(relay, *it->second);
                    myio::log("[{}] Relay {} → {}",
                              cfg_.server_id, relay.id, relay.state ? "ON" : "OFF");
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
                myio::log(stderr, "[{}] Poll error: {}", cfg_.server_id, ex.what());
            }
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
        for (const auto& c : cfg_.covers) {
            if (!c.enabled || c.cover_id != id_str) continue;
            std::string post;
            if      (cmd == "OPEN")  post = std::format("PCA_OFF={}&PCA_ON={}",  c.down.id, c.up.id);
            else if (cmd == "CLOSE") post = std::format("PCA_OFF={}&PCA_ON={}",  c.up.id,   c.down.id);
            else if (cmd == "STOP")  post = std::format("PCA_OFF={}&PCA_OFF={}", c.up.id,   c.down.id);
            else co_return;
            myio::log("[{}] Cover {} → {}", cfg_.server_id, c.cover_id, post);
            co_await client_->async_send_command(post);
            co_return;
        }
        co_return;
    }

    int id = 0;
    if (std::from_chars(id_str.data(), id_str.data() + id_str.size(), id).ec != std::errc{})
        co_return;

    // Light brightness
    if (entity_type == "light" && parts.size() >= 6 && parts[4] == "brightness") {
        // Check enabled
        const bool enabled = std::ranges::any_of(cfg_.pca,
            [id](const auto& e) { return e.id == id && e.enabled; }) ||
            std::ranges::any_of(cfg_.pwm,
            [id](const auto& e) { return e.id == id && e.enabled; });
        if (!enabled) co_return;

        int brightness = 0;
        std::from_chars(payload.data(), payload.data() + payload.size(), brightness);
        const int pct  = static_cast<int>(std::round(brightness / 255.0 * 100.0));
        const auto post = std::format("PCA*{}={}", id, pct);
        myio::log("[{}] Light brightness {} → {}", cfg_.server_id, id, post);
        co_await client_->async_send_command(post);
        co_return;
    }

    // Switch / light ON-OFF
    if (entity_type == "switch" || entity_type == "light") {
        // Find enabled matching entry
        const bool is_pca = std::ranges::any_of(cfg_.pca,
            [id](const auto& e) { return e.id == id && e.enabled; });
        const bool is_relay = std::ranges::any_of(cfg_.relays,
            [id](const auto& e) { return e.id == id && e.enabled; });
        const bool is_pwm = std::ranges::any_of(cfg_.pwm,
            [id](const auto& e) { return e.id == id && e.enabled; });

        std::string post;
        if (is_pca || is_pwm) {
            post = (cmd == "ON") ? std::format("PCA_ON={}", id) : std::format("PCA_OFF={}", id);
        } else if (is_relay) {
            post = (cmd == "ON") ? std::format("r_ON={}", id) : std::format("r_OFF={}", id);
        } else {
            // Not enabled – ignore silently
            co_return;
        }

        myio::log("[{}] {} {} → {}", cfg_.server_id, entity_type, id, post);
        co_await client_->async_send_command(post);
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

    net::steady_timer t(co_await net::this_coro::executor);
    t.expires_after(5s);
    co_await t.async_wait(net::use_awaitable);

    for (auto& srv : servers_)
        co_await srv->publish_discovery();

    for (auto& srv : servers_) {
        net::co_spawn(ioc_,
            [&srv]() -> net::awaitable<void> { co_await srv->poll_loop(); },
            net::detached);
    }

    co_await command_loop();
}

net::awaitable<void> Bridge::command_loop() {
    myio::log("[bridge] Command loop started.");
    while (true) {
        std::exception_ptr caught;
        try {
            auto [topic, payload] = co_await publisher_->async_receive();
            for (auto& srv : servers_)
                co_await srv->handle_command(topic, payload);
        } catch (...) {
            caught = std::current_exception();
        }
        if (caught) {
            try { std::rethrow_exception(caught); }
            catch (const std::exception& ex) {
                myio::log(stderr, "[bridge] Command loop error: {}", ex.what());
            }
            net::steady_timer t(co_await net::this_coro::executor);
            t.expires_after(2s);
            co_await t.async_wait(net::use_awaitable);
        }
    }
}

} // namespace bridge
