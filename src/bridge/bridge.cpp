#include "bridge.hpp"
#include <log.hpp>
#include "async_http_client.hpp"
#include "bridge_rules.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <random>
#include <ranges>
#include <string>

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "dev"
#endif

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
std::string light_percent_topic(const std::string& p, const std::string& s, int id) {
    return std::format("{}/{}/light/{}/percent", p, s, id);
}

// ── HA discovery JSON helpers ─────────────────────────────────────────────────
// No "retain" key in any config: it sets the retain flag of the COMMANDS that
// HA publishes, and a retained /set would be replayed as a command later.
// Every entity gets the shared blocks of add_entity_common (device, origin,
// availability, default_entity_id = "<component>.<unique_id>").

std::string make_switch_discovery(const DiscoveryContext& ctx, const config::PcaConfig& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
    const std::string uid  = std::format("myio_{}_pca_{}", s, e.id);
    const std::string base = std::format("{}/{}/switch/{}", p, s, e.id);
    json::object o;
    o["name"]          = e.name.empty() ? uid : e.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    o["payload_on"]    = "ON";
    o["payload_off"]   = "OFF";
    add_entity_common(o, ctx, "switch", uid);
    return json::serialize(o);
}

std::string make_switch_discovery(const DiscoveryContext& ctx, const config::RelayConfig& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
    const std::string uid  = std::format("myio_{}_relay_{}", s, e.id);
    const std::string base = std::format("{}/{}/switch/{}", p, s, e.id);
    json::object o;
    o["name"]          = e.name.empty() ? uid : e.name;
    o["unique_id"]     = uid;
    o["state_topic"]   = base + "/state";
    o["command_topic"] = base + "/set";
    o["payload_on"]    = "ON";
    o["payload_off"]   = "OFF";
    add_entity_common(o, ctx, "switch", uid);
    return json::serialize(o);
}

std::string make_light_discovery(const DiscoveryContext& ctx, const config::PcaConfig& e,
                                  bool brightness) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
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
    add_entity_common(o, ctx, "light", uid);
    return json::serialize(o);
}

std::string make_light_discovery(const DiscoveryContext& ctx, const config::PwmConfig& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
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
    add_entity_common(o, ctx, "light", uid);
    return json::serialize(o);
}

std::string make_number_discovery(const DiscoveryContext& ctx, const config::PcaConfig& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
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
    add_entity_common(o, ctx, "number", uid);
    return json::serialize(o);
}

std::string make_number_discovery(const DiscoveryContext& ctx, const config::PwmConfig& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
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
    add_entity_common(o, ctx, "number", uid);
    return json::serialize(o);
}

std::string make_cover_discovery(const DiscoveryContext& ctx, const config::CoverConfig& c) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
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
    add_entity_common(o, ctx, "cover", uid);
    return json::serialize(o);
}

std::string make_sensor_discovery(const DiscoveryContext& ctx, const config::SensorConfig& sen) {
    const std::string uid = std::format("myio_{}_sensor_{}", ctx.server_id, sen.id);
    json::object o;
    o["name"]        = sen.name.empty() ? uid : sen.name;
    o["unique_id"]   = uid;
    o["state_topic"] = sensor_state_topic(ctx.prefix, ctx.server_id, sen.id);
    const auto cls = sensor_class(sen.id);
    if (!cls.device_class.empty()) o["device_class"]        = cls.device_class;
    if (!cls.unit.empty())         o["unit_of_measurement"] = cls.unit;
    o["state_class"] = cls.state_class;
    add_entity_common(o, ctx, "sensor", uid);
    return json::serialize(o);
}

// ── Discovery topic helpers ───────────────────────────────────────────────────
// Returns the homeassistant/... discovery topic for a given entity.
// Publishing an empty string to this topic removes the entity from HA.

static std::string_view kind_name(OutputKind kind) {
    switch (kind) {
        case OutputKind::Pca:   return "pca";
        case OutputKind::Relay: return "relay";
        case OutputKind::Pwm:   return "pwm";
    }
    return "";
}

// Writable components keep their historical object id (myio_<srv>_<id>) so the
// existing HA entities stay untouched. Read-only components use the
// kind-qualified id (= unique_id): a bare PWM id (101–113) would otherwise
// collide with a humidity sensor's (101–108) sensor discovery topic.
static std::string output_discovery_topic(const std::string& s, OutputKind kind, int id,
                                          std::string_view component) {
    if (component == "binary_sensor" || component == "sensor")
        return std::format("homeassistant/{}/myio_{}_{}_{}/config",
                           component, s, kind_name(kind), id);
    return std::format("homeassistant/{}/myio_{}_{}/config", component, s, id);
}

// Every component an output of this kind can be published as — the stale
// candidates cleared on a type / writable change.
static std::vector<std::string_view> candidate_components(OutputKind kind) {
    switch (kind) {
        case OutputKind::Pca:   return {"switch", "light", "number", "binary_sensor", "sensor"};
        case OutputKind::Relay: return {"switch", "binary_sensor"};
        case OutputKind::Pwm:   return {"light", "number", "sensor"};
    }
    return {};
}

// Discovery config of a read-only (writable: false) output: no command_topic.
// binary_sensor reads the ON/OFF switch state topic, sensor a percentage.
template <typename Output>
static std::string make_readonly_discovery(const DiscoveryContext& ctx,
                                           OutputKind kind, const Output& e) {
    const auto& p = ctx.prefix;
    const auto& s = ctx.server_id;
    const auto component = ha_component(kind, e.type, false);
    const std::string uid = std::format("myio_{}_{}_{}", s, kind_name(kind), e.id);
    json::object o;
    o["name"]      = e.name.empty() ? uid : e.name;
    o["unique_id"] = uid;
    if (component == "binary_sensor") {
        o["state_topic"] = switch_state_topic(p, s, e.id);
        o["payload_on"]  = "ON";
        o["payload_off"] = "OFF";
        if (!e.device_class.empty()) o["device_class"] = e.device_class;
    } else {
        o["state_topic"] = (command_component(kind, e.type) == "number")
            ? number_state_topic(p, s, e.id)
            : light_percent_topic(p, s, e.id);
        o["unit_of_measurement"] = "%";
    }
    add_entity_common(o, ctx, component, uid);
    return json::serialize(o);
}

static std::string sensor_discovery_topic(const std::string& s, const config::SensorConfig& e) {
    return std::format("homeassistant/sensor/myio_{}_{}/config", s, e.id);
}

static std::string cover_discovery_topic(const std::string& s, const config::CoverConfig& c) {
    return std::format("homeassistant/cover/myio_{}_{}/config", s, c.cover_id);
}

// ── Utility ───────────────────────────────────────────────────────────────────

// Payload as shown in a warning: bounded, so a junk message cannot flood the log.
static std::string payload_for_log(const std::string& payload) {
    constexpr std::size_t max_len = 32;
    if (payload.size() <= max_len) return std::format("'{}'", payload);
    return std::format("'{}…' ({} bytes)", payload.substr(0, max_len), payload.size());
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

// ── Concurrency helper ────────────────────────────────────────────────────────
// Runs a batch of independent, no-argument coroutines concurrently on the given
// executor and waits for all of them to finish. Each task's own exceptions are
// caught so a failing task never leaves the others half-spawned; if one or more
// threw, the first exception is rethrown once every task has completed.
// Used to turn "N sequential awaits, each paying a full round-trip" into
// "N in-flight requests, one round-trip" for independent MQTT publishes.
static net::awaitable<void> run_concurrently(
    net::any_io_executor ex,
    std::vector<std::function<net::awaitable<void>()>> tasks) {
    if (tasks.empty()) co_return;

    using done_channel_t = net::experimental::concurrent_channel<
        void(boost::system::error_code, std::exception_ptr)>;
    done_channel_t done(ex, tasks.size());

    for (auto& task : tasks) {
        net::co_spawn(ex,
            [task = std::move(task), &done]() mutable -> net::awaitable<void> {
                std::exception_ptr ep;
                try {
                    co_await task();
                } catch (...) {
                    ep = std::current_exception();
                }
                co_await done.async_send(boost::system::error_code{}, ep, net::use_awaitable);
            },
            net::detached);
    }

    std::exception_ptr first_error;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        auto [ec, ep] = co_await done.async_receive(net::as_tuple(net::use_awaitable));
        if (ep && !first_error) first_error = ep;
    }
    if (first_error) std::rethrow_exception(first_error);
}

// ── Cover motor-timeout safety net ────────────────────────────────────────────
// Free coroutine (not an immediately-invoked capturing lambda): every value is a
// by-value parameter, so it lives in the coroutine frame for the whole wait.
static net::awaitable<void> cover_motor_timeout(
    std::shared_ptr<ha::IHAPublisher> pub,
    std::string prefix, std::string sid, std::string cov_id, std::string cov_name,
    int end_pos, int timeout_sec, std::shared_ptr<bool> cancelled) {
    try {
        net::steady_timer t(co_await net::this_coro::executor);
        t.expires_after(std::chrono::seconds(timeout_sec));
        co_await t.async_wait(net::use_awaitable);
        if (*cancelled) co_return;  // STOP was pressed, skip end-position publish
        co_await pub->async_publish(
            cover_state_topic(prefix, sid, cov_id), "stopped", true);
        co_await pub->async_publish(
            cover_position_topic(prefix, sid, cov_id), std::to_string(end_pos), true);
        myio::log("[{}] [bridge to broker] Cover {} ({}) → stopped (motor timeout)", sid, cov_id, cov_name);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[{}] Cover {} motor timer error: {}",
                  sid, cov_id, myio::short_what(ex));
    } catch (...) {}
}

// An output that is an endpoint of an ENABLED cover is driven only through its
// cover entity: discovery gives it no own entity and its own /set is dropped.
// Endpoints of a disabled cover are free outputs again.
static bool is_cover_endpoint(const std::vector<config::CoverConfig>& covers,
                              OutputKind kind, int id) {
    if (kind == OutputKind::Pwm) return false;
    const std::string_view t = (kind == OutputKind::Relay) ? "relay" : "pca";
    return std::ranges::any_of(covers, [&](const config::CoverConfig& c) {
        return c.enabled &&
               ((c.up.output_type   == t && c.up.id   == id) ||
                (c.down.output_type == t && c.down.id == id));
    });
}

// ── ServerBridge ──────────────────────────────────────────────────────────────

ServerBridge::ServerBridge(config::ServerConfig               cfg,
                            std::string                        topic_prefix,
                            std::shared_ptr<ha::IHAPublisher>  publisher,
                            std::unique_ptr<myio::IMyIOClient> client,
                            const net::any_io_executor&        ex)
    : cfg_(std::move(cfg))
    , topic_prefix_(std::move(topic_prefix))
    , publisher_(std::move(publisher))
    , client_(std::move(client))
    , cmd_chan_(ex, 1024)
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

DiscoveryContext ServerBridge::discovery_context() const {
    DiscoveryContext ctx;
    ctx.prefix           = topic_prefix_;
    ctx.server_id        = cfg_.server_id;
    ctx.host             = cfg_.host;
    ctx.port             = cfg_.port;
    ctx.firmware_version = firmware_version_;
    ctx.bridge_version   = BRIDGE_VERSION;
    return ctx;
}

std::string ServerBridge::pca_discovery_payload(const config::PcaConfig& e,
                                                std::string_view component) const {
    const auto ctx = discovery_context();
    if (!e.writable)          return make_readonly_discovery(ctx, OutputKind::Pca, e);
    if (component == "light") {
        const bool can_dim = pca_is_pwm_.count(e.id) && pca_is_pwm_.at(e.id);
        return make_light_discovery(ctx, e, can_dim);
    }
    if (component == "number") return make_number_discovery(ctx, e);
    return make_switch_discovery(ctx, e);
}

net::awaitable<bool> ServerBridge::fetch_server_info() {
    try {
        const auto info = co_await client_->async_get_info();
        myio::log("[{}] myIO v{} | {} | serial {} | {} relays, {} PCA | API v{}",
                  cfg_.server_id, info.version, info.cpu, info.serial,
                  info.relay_count, info.pca_count, info.api);
        firmware_version_ = info.version;
        co_return true;
    } catch (const std::exception& ex) {
        last_info_error_ = myio::short_what(ex);
    }
    co_return false;
}

net::awaitable<void> ServerBridge::publish_discovery() {
    const auto& p = topic_prefix_;
    const auto& s = cfg_.server_id;

    // HTTP to the device only on the first discovery, before poll_loop starts
    // (sequential): later discoveries (HA birth, reconnect) would run in
    // parallel with the poll loop's requests, and the W5100 has only 4 sockets.
    // From then on poll_loop owns the device traffic: it keeps pca_is_pwm_
    // current and fetches a still-missing firmware version itself.
    const bool may_query_device = !poll_started_;

    // Firmware version for the device block — fetched before the first
    // discovery so it already carries it; without it the discovery still goes
    // out (no sw_version).
    if (may_query_device && firmware_version_.empty() && !(co_await fetch_server_info()))
        myio::log(stderr, "[{}] Server info unavailable: {}", s, last_info_error_);
    const auto ctx = discovery_context();

    // Fetch initial state to determine PCA hardware capabilities (pwm flag).
    // Only while nothing is known yet; afterwards every poll keeps it current.
    if (may_query_device && pca_is_pwm_.empty()) {
        try {
            const auto state = co_await client_->async_poll();
            for (const auto& pca : state.pca_outputs)
                pca_is_pwm_[pca.id] = pca.is_pwm;
        } catch (const std::exception& ex) {
            myio::log(stderr, "[{}] Could not determine PCA capabilities at discovery: {}",
                      s, myio::short_what(ex));
        }
    }

    // A failed publish is logged and skipped, never aborts the rest: discovery
    // runs at startup, on HA restart and on reconnect, and is simply retried then.
    int         failures = 0;
    std::string first_error;
    auto publish = [&](const std::string& topic, const std::string& payload)
        -> net::awaitable<void> {
        try {
            co_await publisher_->async_publish(topic, payload, true);
        } catch (const std::exception& ex) {
            if (failures++ == 0) first_error = myio::short_what(ex);
        }
    };

    // One output: clear the config topic of every OTHER component it could have
    // been published as (type or writable change — HA would otherwise keep the
    // old entity as an orphan; clearing a never-used topic is harmless), then
    // publish the current config, or "" (undiscovery) when disabled.
    // component "" = cover endpoint: only represented by its cover entity.
    // The payload is built only after the awaited clears: a concurrent poll may
    // have updated pca_is_pwm_ (and re-sent the light discovery) meanwhile, and a
    // payload built earlier would overwrite that with a stale one.
    // No make_payload = disabled (undiscovery).
    auto publish_output = [&](OutputKind kind, int id, std::string_view component,
                              std::function<std::string()> make_payload)
        -> net::awaitable<void> {
        const std::string current = component.empty()
            ? std::string() : output_discovery_topic(s, kind, id, component);
        for (const auto c : candidate_components(kind)) {
            const std::string stale = output_discovery_topic(s, kind, id, c);
            if (stale != current) co_await publish(stale, "");
        }
        if (!current.empty())
            co_await publish(current, make_payload ? make_payload() : std::string());
    };

    for (const auto& e : cfg_.pca) {
        const std::string_view component = is_cover_endpoint(cfg_.covers, OutputKind::Pca, e.id)
            ? std::string_view{} : ha_component(OutputKind::Pca, e.type, e.writable);
        std::function<std::string()> make_payload;
        if (e.enabled && !component.empty())
            make_payload = [&] { return pca_discovery_payload(e, component); };
        co_await publish_output(OutputKind::Pca, e.id, component, std::move(make_payload));
    }

    for (const auto& e : cfg_.relays) {
        const std::string_view component = is_cover_endpoint(cfg_.covers, OutputKind::Relay, e.id)
            ? std::string_view{} : ha_component(OutputKind::Relay, e.type, e.writable);
        std::string payload;
        if (e.enabled && !component.empty())
            payload = e.writable ? make_switch_discovery(ctx, e)
                                 : make_readonly_discovery(ctx, OutputKind::Relay, e);
        co_await publish_output(OutputKind::Relay, e.id, component,
                                [&payload] { return payload; });
    }

    for (const auto& e : cfg_.pwm) {
        const auto component = ha_component(OutputKind::Pwm, e.type, e.writable);
        std::string payload;
        if (e.enabled && !component.empty()) {
            if (!e.writable)                payload = make_readonly_discovery(ctx, OutputKind::Pwm, e);
            else if (component == "number") payload = make_number_discovery(ctx, e);
            else                            payload = make_light_discovery(ctx, e);
        }
        co_await publish_output(OutputKind::Pwm, e.id, component,
                                [&payload] { return payload; });
    }

    for (const auto& e : cfg_.sensors) {
        const std::string dtopic = sensor_discovery_topic(s, e);
        co_await publish(dtopic, e.enabled ? make_sensor_discovery(ctx, e) : std::string());
    }

    for (const auto& c : cfg_.covers) {
        const std::string dtopic = cover_discovery_topic(s, c);
        if (c.enabled) {
            co_await publish(dtopic, make_cover_discovery(ctx, c));
            // First discovery only: reset any stale retained state (e.g.
            // "open"/"closed" from a previous run). Never on a re-send (HA
            // restart, reconnect): that would overwrite the live state.
            // Re-checked right before each seed publish: a poll may publish the
            // real state while an earlier publish is in flight.
            const int before = failures;
            if (!seeded_covers_.contains(c.cover_id))
                co_await publish(cover_state_topic(p, s, c.cover_id), "stopped");
            if (!seeded_covers_.contains(c.cover_id))
                co_await publish(cover_position_topic(p, s, c.cover_id), "50");
            if (failures == before) seeded_covers_.insert(c.cover_id);
        } else {
            co_await publish(dtopic, "");
        }
    }

    if (failures > 0)
        myio::log(stderr, "[{}] Discovery: {} publish(es) failed, first: {}",
                  cfg_.server_id, failures, first_error);
    else
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
        if (should_log_now())
            myio::log("[{}] [{}] PCA {} ({}) → {}%",
                      cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", pca.id, cfg.name, pct);
        co_return;
    }

    // Read-only light: its HA entity is a percentage sensor
    if (cfg.type == config::OutputType::Light && !cfg.writable) {
        const int pct = static_cast<int>(std::round(pca.state / 255.0 * 100.0));
        co_await publisher_->async_publish(
            light_percent_topic(topic_prefix_, cfg_.server_id, pca.id),
            std::to_string(pct), true);
        if (should_log_now())
            myio::log("[{}] [{}] PCA {} ({}) → {}% (read-only)",
                      cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", pca.id, cfg.name, pct);
        co_return;
    }

    const std::string on_off = (pca.state > pca.turn_off) ? "ON" : "OFF";
    const char* tag = syncing_ ? "startup sync" : "myIO to broker";
    if (cfg.type == config::OutputType::Light) {
        co_await publisher_->async_publish(
            light_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
        if (pca.is_pwm) {
            const int pct = static_cast<int>(std::round(pca.state / 255.0 * 100.0));
            co_await publisher_->async_publish(
                light_bright_topic(topic_prefix_, cfg_.server_id, pca.id),
                std::to_string(pca.state), true);
            if (should_log_now())
                myio::log("[{}] [{}] PCA {} ({}) → {} brightness={} ({}%)",
                          cfg_.server_id, tag, pca.id, cfg.name, on_off, pca.state, pct);
        } else {
            if (should_log_now())
                myio::log("[{}] [{}] PCA {} ({}) → {}", cfg_.server_id, tag, pca.id, cfg.name, on_off);
        }
    } else {
        co_await publisher_->async_publish(
            switch_state_topic(topic_prefix_, cfg_.server_id, pca.id), on_off, true);
        if (should_log_now())
            myio::log("[{}] [{}] PCA {} ({}) → {}", cfg_.server_id, tag, pca.id, cfg.name, on_off);
    }
}

net::awaitable<void> ServerBridge::publish_relay_state(const myio::RelayOutput& relay,
                                                         const config::RelayConfig& cfg) {
    const char* on_off = relay.state ? "ON" : "OFF";
    co_await publisher_->async_publish(
        switch_state_topic(topic_prefix_, cfg_.server_id, relay.id), on_off, true);
    if (should_log_now())
        myio::log("[{}] [{}] Relay {} ({}) → {}",
                  cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", relay.id, cfg.name, on_off);
}

net::awaitable<void> ServerBridge::publish_cover_state(const config::CoverConfig& cover,
                                                         int up_state, int down_state, int dir_hint) {
    // A real state is about to go to the broker: a discovery must not seed over
    // it. Marked before the awaits, so a seed racing these publishes sees it; on
    // failure the unchanged poll cache retries the real state anyway.
    seeded_covers_.insert(cover.cover_id);
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
        if (should_log_now())
            myio::log("[{}] [{}] PWM {} ({}) → {}%",
                      cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", pwm.id, cfg.name, pct);
        co_return;
    }

    const std::string on_off = (pwm.state > pwm.turn_off) ? "ON" : "OFF";
    const int pct = static_cast<int>(std::round(pwm.state / 255.0 * 100.0));
    // Read-only light: its HA entity is a percentage sensor
    if (!cfg.writable) {
        co_await publisher_->async_publish(
            light_percent_topic(topic_prefix_, cfg_.server_id, pwm.id),
            std::to_string(pct), true);
        if (should_log_now())
            myio::log("[{}] [{}] PWM {} ({}) → {}% (read-only)",
                      cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", pwm.id, cfg.name, pct);
        co_return;
    }
    co_await publisher_->async_publish(
        light_state_topic(topic_prefix_, cfg_.server_id, pwm.id), on_off, true);
    co_await publisher_->async_publish(
        light_bright_topic(topic_prefix_, cfg_.server_id, pwm.id),
        std::to_string(pwm.state), true);
    if (should_log_now())
        myio::log("[{}] [{}] PWM {} ({}) → {} brightness={} ({}%)",
                  cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", pwm.id, cfg.name, on_off, pwm.state, pct);
}

net::awaitable<void> ServerBridge::publish_sensor_state(const myio::Sensor& sensor,
                                                          const config::SensorConfig& cfg,
                                                          bool do_log) {
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
    if (do_log)
        myio::log("[{}] [{}] Sensor {} ({}) → {}",
                  cfg_.server_id, syncing_ ? "startup sync" : "myIO to broker", sensor.id, cfg.name, val);
}

// ── Resync ────────────────────────────────────────────────────────────────────

void ServerBridge::schedule_resync() {
    for (auto& [id, v] : last_pca_state_)   v = -1;
    for (auto& [id, v] : last_relay_state_) v = -1;
    for (auto& [id, v] : last_pwm_state_)   v = -1;
    for (auto& [id, v] : last_sensor_val_)        v = std::numeric_limits<double>::max();
    for (auto& [id, v] : last_sensor_logged_val_) v = std::numeric_limits<double>::max();
    published_available_.reset();  // republish the server availability too
    syncing_ = true;
    myio::log("[{}] Broker reconnected – resync scheduled.", cfg_.server_id);
}

// ── Availability ──────────────────────────────────────────────────────────────

net::awaitable<void> ServerBridge::update_availability(bool online) {
    if (published_available_ == online) co_return;
    try {
        co_await publisher_->async_publish(
            server_availability_topic(topic_prefix_, cfg_.server_id),
            online ? kAvailabilityOnline : kAvailabilityOffline, true);
        published_available_ = online;
        myio::log("[{}] [bridge to broker] Availability → {}", cfg_.server_id,
                  online ? kAvailabilityOnline : kAvailabilityOffline);
    } catch (const std::exception& ex) {
        // Cache unchanged → retried after the next poll.
        myio::log(stderr, "[{}] Availability publish failed: {}",
                  cfg_.server_id, myio::short_what(ex));
    }
}

// ── PCA capabilities ──────────────────────────────────────────────────────────

net::awaitable<void> ServerBridge::update_pca_capabilities(const myio::ServerState& state) {
    std::vector<int> changed;
    for (const auto& pca : state.pca_outputs) {
        const auto it = pca_is_pwm_.find(pca.id);
        if (it != pca_is_pwm_.end() && it->second == pca.is_pwm) continue;
        pca_is_pwm_[pca.id] = pca.is_pwm;
        changed.push_back(pca.id);
    }

    for (const auto& e : cfg_.pca) {
        if (!e.enabled || !e.writable || !std::ranges::contains(changed, e.id)) continue;
        if (is_cover_endpoint(cfg_.covers, OutputKind::Pca, e.id)) continue;
        const auto component = ha_component(OutputKind::Pca, e.type, e.writable);
        if (component != "light") continue;
        try {
            co_await publisher_->async_publish(
                output_discovery_topic(cfg_.server_id, OutputKind::Pca, e.id, component),
                pca_discovery_payload(e, component), true);
            myio::log("[{}] PCA {} ({}) dimmable={} – light discovery re-sent.",
                      cfg_.server_id, e.id, e.name, pca_is_pwm_.at(e.id));
        } catch (const std::exception& ex) {
            // Forget the flag so the next poll detects the change and retries.
            pca_is_pwm_.erase(e.id);
            myio::log(stderr, "[{}] PCA {} light discovery re-send failed: {}",
                      cfg_.server_id, e.id, myio::short_what(ex));
        }
    }
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

    // /info.json (firmware version) is fetched by the first publish_discovery;
    // only if that failed is it retried here, after a successful poll. From
    // now on discovery does no device HTTP (see publish_discovery).
    poll_started_ = true;
    myio::log("[{}] Poll loop started, interval={}s",
              cfg_.server_id, cfg_.poll_interval_sec);

    // Sentinel init: force first poll to publish current state regardless of value.
    // Without this, state=0 (OFF) would match the default-initialized last=0 → no publish.
    for (const auto& [id, _] : pca_map)    last_pca_state_[id]   = -1;
    for (const auto& [id, _] : relay_map)  last_relay_state_[id] = -1;
    for (const auto& [id, _] : pwm_map)    last_pwm_state_[id]   = -1;
    for (const auto& [id, _] : sensor_map) last_sensor_val_[id]  = std::numeric_limits<double>::max();
    // Covers too (key = -(up.id)): the discovery seeds their state only once, so
    // the poll — also after a resync — must republish the real one.
    for (const auto& c : cfg_.covers) {
        if (!c.enabled) continue;
        if (c.up.output_type == "relay" || c.down.output_type == "relay")
            last_relay_state_[-(c.up.id)] = -1;
        if (c.up.output_type == "pca" || c.down.output_type == "pca")
            last_pca_state_[-(c.up.id)] = -1;
    }
    syncing_ = true;

    // While the device is unreachable (e.g. the multi-minute W5100 lockup),
    // probing it at the normal rate only ties up its sockets — back off instead.
    int consecutive_errors = 0;

    while (true) {
        std::exception_ptr caught;
        try {
            const auto state = co_await client_->async_poll();
            co_await update_pca_capabilities(state);

            // Publishes for this cycle are collected here and fired concurrently
            // at the end, instead of one at a time — matters most right after a
            // broker reconnect, when every entity resyncs in the same cycle.
            // Every last_* cache entry below is updated only AFTER its publish
            // succeeded (inside the task): a failed publish leaves the old value,
            // so the next poll sees the change again and retries it.
            std::vector<std::function<net::awaitable<void>()>> pending;
            // Both endpoints of a cover are visited; schedule its publish once.
            std::unordered_set<int> scheduled_pca_covers, scheduled_relay_covers;

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

                    if (last_pca_state_[key] != combined &&
                        scheduled_pca_covers.insert(key).second) {
                        if      (up_state > 0)   last_cover_dir_[key] = +1;
                        else if (down_state > 0) last_cover_dir_[key] = -1;
                        const int dir = last_cover_dir_.count(key) ? last_cover_dir_.at(key) : 0;
                        pending.push_back([this, cover, up_state, down_state, dir, key, combined]()
                            -> net::awaitable<void> {
                            co_await publish_cover_state(*cover, up_state, down_state, dir);
                            last_pca_state_[key] = combined;
                        });
                    }
                    continue;
                }

                const auto it = pca_map.find(pca.id);
                if (it == pca_map.end()) continue;

                if (last_pca_state_[pca.id] != pca.state) {
                    pending.push_back([this, pca, cfg = it->second]() -> net::awaitable<void> {
                        co_await publish_pca_state(pca, *cfg);
                        last_pca_state_[pca.id] = pca.state;
                    });
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

                    if (last_relay_state_[key] != combined &&
                        scheduled_relay_covers.insert(key).second) {
                        if      (up_state > 0)   last_cover_dir_[key] = +1;
                        else if (down_state > 0) last_cover_dir_[key] = -1;
                        const int dir = last_cover_dir_.count(key) ? last_cover_dir_.at(key) : 0;
                        pending.push_back([this, cover, up_state, down_state, dir, key, combined]()
                            -> net::awaitable<void> {
                            co_await publish_cover_state(*cover, up_state, down_state, dir);
                            last_relay_state_[key] = combined;
                        });
                    }
                    continue;
                }

                const auto it = relay_map.find(relay.id);
                if (it == relay_map.end()) continue;
                const int relay_state = relay.state ? 1 : 0;
                if (last_relay_state_[relay.id] != relay_state) {
                    pending.push_back([this, relay, relay_state, cfg = it->second]()
                        -> net::awaitable<void> {
                        co_await publish_relay_state(relay, *cfg);
                        last_relay_state_[relay.id] = relay_state;
                    });
                }
            }

            for (const auto& pwm : state.pwm_outputs) {
                const auto it = pwm_map.find(pwm.id);
                if (it == pwm_map.end()) continue;
                if (last_pwm_state_[pwm.id] != pwm.state) {
                    pending.push_back([this, pwm, cfg = it->second]() -> net::awaitable<void> {
                        co_await publish_pwm_state(pwm, *cfg);
                        last_pwm_state_[pwm.id] = pwm.state;
                    });
                }
            }

            for (const auto& sensor : state.sensors) {
                const auto it = sensor_map.find(sensor.id);
                if (it == sensor_map.end()) continue;
                double val = 0.0;
                if      (sensor.temperature) val = *sensor.temperature;
                else if (sensor.humidity)    val = *sensor.humidity;
                else if (sensor.power)       val = *sensor.power;
                else if (sensor.voltage)     val = *sensor.voltage;
                else if (sensor.current)     val = *sensor.current;
                else if (sensor.energy)      val = *sensor.energy;
                else {
                    auto& last = last_sensor_val_[sensor.id];
                    if (last != std::numeric_limits<double>::max()) {
                        myio::log(stderr, "[{}] Sensor {} ({}) error value – not published",
                                  cfg_.server_id, sensor.id, it->second->name);
                        last = std::numeric_limits<double>::max();
                    }
                    continue;
                }
                if (std::abs(last_sensor_val_[sensor.id] - val) > 0.005) {
                    const double last_logged = last_sensor_logged_val_[sensor.id];
                    const double thr = (it->second->log_threshold >= 0.0)
                                         ? it->second->log_threshold
                                         : cfg_.sensor_log_threshold;
                    const bool do_log = should_log_now() &&
                                        (thr <= 0.0 ||
                                         std::abs(last_logged - val) >= thr);
                    const bool update_logged = do_log || syncing_;
                    pending.push_back([this, sensor, cfg = it->second, do_log, update_logged, val]()
                        -> net::awaitable<void> {
                        co_await publish_sensor_state(sensor, *cfg, do_log);
                        last_sensor_val_[sensor.id] = val;
                        if (update_logged) last_sensor_logged_val_[sensor.id] = val;
                    });
                }
            }

            // A broker-side failure is not a device poll error: log it and keep
            // the normal poll rate (no backoff) — the unchanged caches make the
            // next cycle retry exactly the values that did not get through.
            if (!pending.empty()) {
                try {
                    co_await run_concurrently(ex, std::move(pending));
                } catch (const std::exception& e) {
                    myio::log(stderr, "[{}] State publish error (retry next poll): {}",
                              cfg_.server_id, myio::short_what(e));
                }
            }

        } catch (...) {
            caught = std::current_exception();
        }
        auto delay = std::chrono::seconds(cfg_.poll_interval_sec);
        if (caught) {
            ++consecutive_errors;
            const int shift = std::min(consecutive_errors, 4);
            delay = std::min(delay * (1 << shift), std::chrono::seconds(120));
            try { std::rethrow_exception(caught); }
            catch (const std::exception& ex) {
                std::string msg = myio::short_what(ex);
                if (msg == last_poll_error_) {
                    myio::log(stderr, "[{}] Poll error: (same as previous, retry in {}s)",
                              cfg_.server_id, delay.count());
                } else {
                    last_poll_error_ = msg;
                    myio::log(stderr, "[{}] Poll error: {}", cfg_.server_id, msg);
                }
            }
        } else {
            if (consecutive_errors > 0)
                myio::log("[{}] Poll recovered after {} failed attempt(s).",
                          cfg_.server_id, consecutive_errors);
            consecutive_errors = 0;
            last_poll_error_.clear();
            syncing_ = false;
        }
        // A single failed poll (short W5100 hiccup) must not flap every entity to
        // "unavailable": offline only after several consecutive failures.
        constexpr int kOfflineAfterFailedPolls = 3;
        if (!caught)
            co_await update_availability(true);
        else if (consecutive_errors >= kOfflineAfterFailedPolls)
            co_await update_availability(false);

        // Firmware version still unknown (startup fetch failed): ask the device
        // now, sequentially after the poll, and re-send the discovery so the
        // device block gets its sw_version. A repeated error is logged once.
        if (!caught && firmware_version_.empty()) {
            const std::string prev_error = last_info_error_;
            if (co_await fetch_server_info()) {
                co_await publish_discovery();
            } else if (last_info_error_ != prev_error) {
                myio::log(stderr, "[{}] Server info unavailable: {}",
                          cfg_.server_id, last_info_error_);
            }
        }

        net::steady_timer timer(ex);
        timer.expires_after(delay);
        co_await timer.async_wait(net::use_awaitable);
    }
}

// ── Command handler ───────────────────────────────────────────────────────────
// Only enabled entities receive commands. Disabled entities are silently ignored.
// Everything else that is not a valid command — wrong component for the
// output's configured type, read-only output, cover endpoint, malformed or
// empty payload (a retained-clear) — is dropped with a warning: no POST, and
// no state publish. Only canonical values are ever published as state.

std::optional<PreparedCommand>
ServerBridge::prepare_command(const std::string& topic,
                              const std::string& payload) {
    const auto parts = split_topic(topic);
    if (parts.size() < 5) return std::nullopt;
    if (parts[1] != cfg_.server_id) return std::nullopt;

    const std::string& entity_type = parts[2];
    const std::string& id_str      = parts[3];

    auto drop = [&](std::string_view reason) -> std::optional<PreparedCommand> {
        myio::log(stderr, "[{}] [broker to myIO] Command dropped ({}): {} {}",
                  cfg_.server_id, reason, topic, payload_for_log(payload));
        return std::nullopt;
    };

    // Cover command
    if (entity_type == "cover") {
        if (parts.size() != 5 || parts[4] != "set") return std::nullopt;
        for (const auto& c : cfg_.covers) {
            if (!c.enabled || c.cover_id != id_str) continue;
            const auto cover_cmd = parse_cover_command(payload);
            if (!cover_cmd) return drop("invalid cover payload");
            const std::string cmd = (*cover_cmd == CoverCommand::Open)  ? "OPEN"
                                  : (*cover_cmd == CoverCommand::Close) ? "CLOSE"
                                                                        : "STOP";
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
            else                     post = ep_off(c.up)   + "&" + ep_off(c.down);
            myio::log("[{}] [broker to myIO] Cover {} ({}) → {}", cfg_.server_id, c.cover_id, c.name, post);

            const config::CoverConfig c_copy = c;
            PreparedCommand pcmd;
            pcmd.post = std::move(post);
            pcmd.publish_state = [this, cmd, c_copy]() -> net::awaitable<void> {
            const config::CoverConfig& c = c_copy;

            // Optimistic state publish + direction tracking.
            // The poll interval (30 s) is longer than most motor runs, so direction
            // and state must be set from the command, not from polling.
            // The poll stores covers as combined = up_state*1000 + down_state.
            // For relays the on-value is exactly 1, so the optimistic value can
            // match what the next poll computes (no duplicate republish). PCA
            // on-values are 0–255 and unknown here → use the -1 sentinel, which
            // just forces a (harmless) republish on the next poll.
            // The cache is written only after the publish succeeded; if it threw,
            // the old cache value makes the next poll republish the real state.
            const int key = -(c.up.id);
            if (cmd == "OPEN") {
                last_cover_dir_[key] = +1;
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "opening", true);
                if (c.up.output_type == "relay") last_relay_state_[key] = 1000;
                else                             last_pca_state_[key]   = -1;
                myio::log("[{}] [bridge to broker] Cover {} ({}) → opening", cfg_.server_id, c.cover_id, c.name);
            } else if (cmd == "CLOSE") {
                last_cover_dir_[key] = -1;
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "closing", true);
                if (c.up.output_type == "relay") last_relay_state_[key] = 1;
                else                             last_pca_state_[key]   = -1;
                myio::log("[{}] [bridge to broker] Cover {} ({}) → closing", cfg_.server_id, c.cover_id, c.name);
            } else if (cmd == "STOP") {
                // The motor is stopped (POST succeeded): cancel the end-position
                // timer even if the state publish below fails.
                if (last_cover_cancel_.count(key))
                    *last_cover_cancel_[key] = true;
                co_await publisher_->async_publish(
                    cover_state_topic(topic_prefix_, cfg_.server_id, c.cover_id), "stopped", true);
                co_await publisher_->async_publish(
                    cover_position_topic(topic_prefix_, cfg_.server_id, c.cover_id), "50", true);
                myio::log("[{}] [bridge to broker] Cover {} ({}) → stopped", cfg_.server_id, c.cover_id, c.name);
            }

            // After OPEN/CLOSE, the motor stops on its own when it reaches the end stop.
            // The poll may not catch this within the 30 s interval, leaving HA stuck in
            // "opening"/"closing". Spawn a timer that publishes "stopped" after 90 s as
            // a safety net. If the user presses STOP first, or the poll catches it sooner,
            // this publish is a harmless duplicate.
            if (cmd == "OPEN" || cmd == "CLOSE") {
                if (c.motor_timeout_sec > 0) {
                    const int end_pos = (cmd == "OPEN") ? 100 : 0;
                    // Supersede the previous motor-timeout timer: without this a
                    // rapid second OPEN/CLOSE leaves the first timer live, and it
                    // would publish a premature "stopped" mid-movement.
                    if (const auto it = last_cover_cancel_.find(key);
                        it != last_cover_cancel_.end() && it->second)
                        *it->second = true;
                    auto flag = std::make_shared<bool>(false);
                    last_cover_cancel_[key] = flag;
                    net::co_spawn(co_await net::this_coro::executor,
                        cover_motor_timeout(publisher_, topic_prefix_, cfg_.server_id,
                                            c.cover_id, c.name, end_pos,
                                            c.motor_timeout_sec, flag),
                        net::detached);
                }
            }
                co_return;
            };
            return pcmd;
        }
        return std::nullopt;
    }

    // Every other command addresses one numbered output:
    //   {prefix}/{srv}/{switch|light|number}/{id}/set  or  {prefix}/{srv}/light/{id}/brightness/set
    // Anything else (state topics, unknown components) is not a command.
    const bool is_set        = parts.size() == 5 && parts[4] == "set";
    const bool is_brightness = parts.size() == 6 && parts[4] == "brightness" && parts[5] == "set";
    if (!is_set && !is_brightness) return std::nullopt;
    if (entity_type != "switch" && entity_type != "light" && entity_type != "number")
        return std::nullopt;
    if (is_brightness && entity_type != "light") return std::nullopt;

    const auto id_opt = parse_int_in_range(id_str, 1, std::numeric_limits<int>::max());
    if (!id_opt) return std::nullopt;
    const int id = *id_opt;

    // Exact type match: the topic's component must be the one that owns the
    // output's configured type (lookup order PCA, PWM, relay as before).
    struct Target {
        OutputKind  kind;
        bool        writable;
        std::string name;
    };
    std::optional<Target> target;
    bool other_type = false;  // an enabled output with this id exists, but of another type
    auto consider = [&](OutputKind kind, const auto& list) {
        for (const auto& e : list) {
            if (e.id != id || !e.enabled) continue;
            const bool owns = !is_cover_endpoint(cfg_.covers, kind, e.id) &&
                              command_component(kind, e.type) == entity_type;
            if (!owns)        other_type = true;
            else if (!target) target = Target{kind, e.writable, e.name};
        }
    };
    consider(OutputKind::Pca,   cfg_.pca);
    consider(OutputKind::Pwm,   cfg_.pwm);
    consider(OutputKind::Relay, cfg_.relays);

    if (!target) {
        if (other_type) return drop("component does not match the output's configured type");
        return std::nullopt;  // unknown or disabled output
    }
    // Second line of defence behind the missing command_topic in discovery.
    if (!target->writable) return drop("read-only output (writable: false)");

    const bool is_pca = (target->kind == OutputKind::Pca);
    const std::string& dev_name = target->name;

    // Number value (0–100, passed through to PCA*/fet* as percentage)
    if (entity_type == "number") {
        const auto value = parse_int_in_range(payload, 0, 100);
        if (!value) return drop("number payload must be an integer 0..100");

        const std::string post = is_pca
            ? std::format("PCA*{}={}", id,       *value)
            : std::format("fet*{}={}", id - 100, *value);
        myio::log("[{}] [broker to myIO] Number {} ({}) → {}", cfg_.server_id, id, dev_name, post);
        PreparedCommand pcmd;
        pcmd.post = post;
        return pcmd;
    }

    // Light brightness
    if (is_brightness) {
        if (is_pca && !(pca_is_pwm_.count(id) && pca_is_pwm_.at(id)))
            return drop("light is not dimmable");
        const auto value = parse_int_in_range(payload, 0, 255);
        if (!value) return drop("brightness payload must be an integer 0..255");
        const int brightness = *value;

        const std::string post = is_pca
            ? std::format("PCA*{}={}", id, brightness)
            : std::format("fet*{}={}", id - 100, brightness);
        myio::log("[{}] [broker to myIO] Light brightness {} ({}) → {} ({}%)",
                  cfg_.server_id, id, dev_name, post,
                  static_cast<int>(std::round(brightness / 255.0 * 100.0)));

        const bool        bright_is_pca = is_pca;
        const std::string dev_name_copy = dev_name;
        PreparedCommand pcmd;
        pcmd.post = post;
        pcmd.publish_state =
            [this, id, brightness, bright_is_pca, dev_name_copy]() -> net::awaitable<void> {
            // Optimistic state publish — like ON/OFF, don't wait for next poll
            const std::string on_off = (brightness > 0) ? "ON" : "OFF";
            co_await publisher_->async_publish(
                light_state_topic(topic_prefix_, cfg_.server_id, id), on_off, true);
            co_await publisher_->async_publish(
                light_bright_topic(topic_prefix_, cfg_.server_id, id),
                std::to_string(brightness), true);
            if (bright_is_pca) last_pca_state_[id] = brightness;
            else               last_pwm_state_[id] = brightness;
            myio::log("[{}] [bridge to broker] Light brightness {} ({}) → {} ({}%)",
                      cfg_.server_id, id, dev_name_copy, brightness,
                      static_cast<int>(std::round(brightness / 255.0 * 100.0)));
            co_return;
        };
        return pcmd;
    }

    // Switch / light ON-OFF
    const auto on = parse_on_off(payload);
    if (!on) return drop("payload must be ON or OFF");
    const std::string cmd = *on ? "ON" : "OFF";

    std::string post;
    switch (target->kind) {
        case OutputKind::Pca:   post = std::format("{}={}", *on ? "PCA_ON" : "PCA_OFF", id);    break;
        case OutputKind::Pwm:   post = std::format("{}={}", *on ? "f_ON"   : "f_OFF",   id - 100); break;
        case OutputKind::Relay: post = std::format("{}={}", *on ? "r_ON"   : "r_OFF",   id);    break;
    }
    myio::log("[{}] [broker to myIO] {} {} ({}) → {}", cfg_.server_id, entity_type, id, dev_name, post);

    // The component equals the topic's (exact match above), so is its state topic.
    const std::string stopic = (entity_type == "light")
        ? light_state_topic(topic_prefix_, cfg_.server_id, id)
        : switch_state_topic(topic_prefix_, cfg_.server_id, id);
    const std::string et       = entity_type;
    const std::string name_cpy = dev_name;
    PreparedCommand pcmd;
    pcmd.post = post;
    pcmd.publish_state = [this, id, cmd, et, stopic, name_cpy]() -> net::awaitable<void> {
        // Publish optimistic state immediately so HA doesn't revert to the
        // old retained state while waiting for the next poll cycle.
        co_await publisher_->async_publish(stopic, cmd, true);
        myio::log("[{}] [bridge to broker] {} {} ({}) → {}", cfg_.server_id, et, id, name_cpy, cmd);
    };
    return pcmd;
}

// ── enqueue + batched command worker ──────────────────────────────────────────

net::awaitable<void> ServerBridge::enqueue_if_mine(const std::string& topic,
                                                   const std::string& payload) {
    const auto parts = split_topic(topic);
    if (parts.size() < 2 || parts[1] != cfg_.server_id) co_return;
    cmd_arrival_times_.push_back(std::chrono::steady_clock::now());
    auto [ec] = co_await cmd_chan_.async_send(
        boost::system::error_code{}, topic, payload,
        net::as_tuple(net::use_awaitable));
    if (ec) {
        cmd_arrival_times_.pop_back();  // send failed – nothing actually queued
        myio::log(stderr, "[{}] Command enqueue failed: {}",
                  cfg_.server_id, ec.message());
    }
}

net::awaitable<void> ServerBridge::process_command_batch(
    const std::vector<std::pair<std::string, std::string>>& batch) {
    std::vector<PreparedCommand> prepared;
    prepared.reserve(batch.size());
    std::string joined;
    for (const auto& [topic, payload] : batch) {
        auto pc = prepare_command(topic, payload);
        if (!pc) continue;
        if (!pc->post.empty()) {
            if (!joined.empty()) joined += '&';
            joined += pc->post;
        }
        prepared.push_back(std::move(*pc));
    }
    if (!joined.empty()) {
        if (prepared.size() > 1)
            myio::log("[{}] [broker to myIO] batch ({} cmd) → {}",
                      cfg_.server_id, prepared.size(), joined);
        co_await client_->async_send_command(joined);
    }
    // Publish optimistic state only after the POST returns. If the POST threw,
    // we never get here (the worker catches it), so HA is not told about a state
    // change the device never received. A failed state publish is logged and
    // does not skip the remaining ones; the next poll republishes the truth.
    for (auto& pc : prepared) {
        if (!pc.publish_state) continue;
        try {
            co_await pc.publish_state();
        } catch (const std::exception& ex) {
            myio::log(stderr, "[{}] [bridge to broker] State publish failed: {}",
                      cfg_.server_id, myio::short_what(ex));
        }
    }
}

net::awaitable<void> ServerBridge::command_worker() {
    // After the first queued command, wait briefly so sibling commands from the
    // same HA action land and go out in a single POST. Configurable per server
    // via bridge.json "coalesce_window_ms" (default 5 ms); every single command
    // waits this long, so keep it just above the observed sibling spread.
    // See README "Command latency & TCP delayed-ACK" for how to size it.
    const auto coalesce_window = std::chrono::milliseconds(cfg_.coalesce_window_ms);
    auto ex = co_await net::this_coro::executor;
    myio::log("[{}] Command worker started.", cfg_.server_id);

    for (;;) {
        // Block until the first command arrives.
        auto [ec, topic, payload] =
            co_await cmd_chan_.async_receive(net::as_tuple(net::use_awaitable));
        if (ec) co_return;  // channel closed → shut down

        const auto t0 = cmd_arrival_times_.empty()
            ? std::chrono::steady_clock::now() : cmd_arrival_times_.front();
        if (!cmd_arrival_times_.empty()) cmd_arrival_times_.pop_front();

        std::vector<std::pair<std::string, std::string>> batch;
        batch.emplace_back(std::move(topic), std::move(payload));

        net::steady_timer timer(ex);
        timer.expires_after(coalesce_window);
        co_await timer.async_wait(net::use_awaitable);

        // Drain everything currently queued into this batch.
        for (;;) {
            std::string t2, p2;
            const bool got = cmd_chan_.try_receive(
                [&](boost::system::error_code, std::string tt, std::string pp) {
                    t2 = std::move(tt);
                    p2 = std::move(pp);
                });
            if (!got) break;

            // Diagnostic: how long after the batch's first command this sibling
            // actually arrived. Sizes the coalesce window from real HA traffic
            // instead of a guess — see how close arrivals cluster to 0 ms vs.
            // how close they cut it to the full window.
            if (!cmd_arrival_times_.empty()) {
                const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    cmd_arrival_times_.front() - t0).count();
                cmd_arrival_times_.pop_front();
                myio::log("[{}] [coalesce] sibling +{}ms after batch start", cfg_.server_id, gap_ms);
            }

            batch.emplace_back(std::move(t2), std::move(p2));
        }

        if (batch.size() > 1)
            myio::log("[{}] [coalesce] batch closed: {} commands within {}ms window",
                      cfg_.server_id, batch.size(), coalesce_window.count());

        try {
            co_await process_command_batch(batch);
            const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            myio::log("[{}] [latency] HA→myIO {}ms ({} cmd)", cfg_.server_id, total_ms, batch.size());
        } catch (const std::exception& e) {
            const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            myio::log(stderr, "[{}] Command batch error after {}ms: {}",
                      cfg_.server_id, total_ms, myio::short_what(e));
        } catch (...) {
            myio::log(stderr, "[{}] Command batch error: unknown", cfg_.server_id);
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
            std::make_unique<myio::AsyncHttpClient>(hcfg, ioc_.get_executor()),
            ioc_.get_executor()));
    }
}

std::vector<std::function<net::awaitable<void>()>> Bridge::discovery_tasks() {
    std::vector<std::function<net::awaitable<void>()>> tasks;
    tasks.reserve(servers_.size());
    for (auto& srv : servers_)
        tasks.push_back([s = srv.get()]() -> net::awaitable<void> { return s->publish_discovery(); });
    return tasks;
}

net::awaitable<void> Bridge::publish_all_discovery() {
    try {
        co_await publisher_->async_publish(
            bridge_status_discovery_topic(),
            make_bridge_status_discovery(cfg_.mqtt.topic_prefix, BRIDGE_VERSION), true);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[bridge] Bridge discovery publish failed: {}", myio::short_what(ex));
    }
    // All servers' discovery is independent — publish it concurrently instead
    // of waiting for each server's full discovery (incl. its info fetch and
    // capability poll) to finish before starting the next one.
    co_await run_concurrently(co_await net::this_coro::executor, discovery_tasks());
}

net::awaitable<void> Bridge::publish_bridge_online() {
    try {
        co_await publisher_->async_publish(
            bridge_availability_topic(cfg_.mqtt.topic_prefix), kAvailabilityOnline, true);
        myio::log("[bridge] [bridge to broker] Availability → {}", kAvailabilityOnline);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[bridge] Availability publish failed: {}", myio::short_what(ex));
    }
}

void Bridge::schedule_birth_discovery() {
    if (birth_discovery_pending_) {
        myio::log("[bridge] HA birth – discovery re-send already pending, coalesced.");
        return;
    }
    birth_discovery_pending_ = true;

    // Random 1–10 s: HA (and every other MQTT integration) is still starting up
    // right after its birth message; spreading the re-send avoids the burst.
    static thread_local std::mt19937 rng{std::random_device{}()};
    const auto delay = std::chrono::milliseconds(
        std::uniform_int_distribution<int>(1000, 10000)(rng));
    myio::log("[bridge] HA restarted – republishing discovery in {}ms.", delay.count());

    net::co_spawn(ioc_,
        [this, delay]() -> net::awaitable<void> {
            net::steady_timer t(co_await net::this_coro::executor);
            t.expires_after(delay);
            co_await t.async_wait(net::use_awaitable);
            // Cleared before the re-send: a birth arriving during it schedules
            // a new one instead of being lost.
            birth_discovery_pending_ = false;
            co_await publish_all_discovery();
        },
        [](std::exception_ptr ep) {
            if (!ep) return;
            try { std::rethrow_exception(ep); }
            catch (const std::exception& ex) {
                myio::log(stderr, "[bridge] Discovery re-send error: {}", myio::short_what(ex));
            } catch (...) {}
        });
}

net::awaitable<void> Bridge::shutdown() {
    stopping_ = true;
    try {
        co_await publisher_->async_publish(
            bridge_availability_topic(cfg_.mqtt.topic_prefix), kAvailabilityOffline, true);
        myio::log("[bridge] [bridge to broker] Availability → {}", kAvailabilityOffline);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[bridge] Availability publish failed: {}", myio::short_what(ex));
    }
    co_await publisher_->async_disconnect();
}

net::awaitable<void> Bridge::run() {
    co_await publisher_->async_connect();
    co_await publisher_->async_subscribe(cfg_.mqtt.topic_prefix + "/#");
    co_await publisher_->async_subscribe("homeassistant/status");
    // Overrides the retained "offline" Will of a previous crash.
    co_await publish_bridge_online();

    co_await publish_all_discovery();

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

    for (auto& srv : servers_) {
        net::co_spawn(ioc_, srv->command_worker(),
            [sid = srv->server_id()](std::exception_ptr ep) {
                if (ep) {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& ex) {
                        myio::log(stderr, "[{}] Command worker crashed: {}",
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
        bool session_expired = false;
        try {
            auto [topic, payload] = co_await publisher_->async_receive();

            if (topic == "homeassistant/status" && payload == "online") {
                schedule_birth_discovery();
                continue;
            }

            for (auto& srv : servers_)
                co_await srv->enqueue_if_mine(topic, payload);
        } catch (const ha::SessionExpired&) {
            // Broker restarted without keeping the session: the connection is
            // already re-established, only the subscriptions are gone. Restore
            // them immediately — every second of delay is a deaf command channel.
            session_expired = true;
        } catch (...) {
            caught = std::current_exception();
        }
        // Requested stop: the disconnect aborts the receive — not an error.
        if (stopping_) co_return;
        if (caught || session_expired) {
            if (session_expired) {
                myio::log("[bridge] MQTT session expired – restoring subscriptions.");
            } else {
                try { std::rethrow_exception(caught); }
                catch (const std::exception& ex) {
                    myio::log(stderr, "[bridge] Command loop error: {}", myio::short_what(ex));
                }
                net::steady_timer t(co_await net::this_coro::executor);
                t.expires_after(30s);
                co_await t.async_wait(net::use_awaitable);
                if (stopping_) co_return;
            }
            // The broker may have published our Will ("offline") on the
            // connection loss. Restored before the resubscribe: state publishing
            // works even while SUBACK keeps failing, so the entities must not
            // stay unavailable meanwhile. The resync republishes the server
            // availability.
            co_await publish_bridge_online();
            // Retry until the subscriptions are back (incl. a SUBACK refusal):
            // giving up would leave the bridge connected but deaf to commands.
            for (;;) {
                try {
                    co_await publisher_->async_subscribe(cfg_.mqtt.topic_prefix + "/#");
                    co_await publisher_->async_subscribe("homeassistant/status");
                    break;
                } catch (const std::exception& ex) {
                    if (stopping_) co_return;
                    myio::log(stderr, "[bridge] Resubscribe failed (retry in 30s): {}",
                              myio::short_what(ex));
                }
                net::steady_timer t(co_await net::this_coro::executor);
                t.expires_after(30s);
                co_await t.async_wait(net::use_awaitable);
                if (stopping_) co_return;
            }
            try {
                myio::log("[bridge] Reconnected – republishing discovery and scheduling state resync.");
                for (auto& srv : servers_)
                    srv->schedule_resync();
                co_await publish_all_discovery();
            } catch (const std::exception& ex) {
                myio::log(stderr, "[bridge] Reconnect setup error: {}", myio::short_what(ex));
            }
        }
    }
}

} // namespace bridge
