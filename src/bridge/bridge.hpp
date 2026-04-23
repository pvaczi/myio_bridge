#pragma once

#include "config_types.hpp"
#include "IHAPublisher.hpp"
#include "IMyIOClient.hpp"
#include <types.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace net = boost::asio;

namespace bridge {

// ── MQTT topic helpers ────────────────────────────────────────────────────────
// Format: {prefix}/{server_id}/{type}/{id}/state
// Example: "myio/lighting/switch/2001/state"

std::string switch_state_topic   (const std::string& prefix, const std::string& srv, int id);
std::string switch_cmd_topic     (const std::string& prefix, const std::string& srv, int id);
std::string light_state_topic    (const std::string& prefix, const std::string& srv, int id);
std::string light_bright_topic   (const std::string& prefix, const std::string& srv, int id);
std::string light_cmd_topic      (const std::string& prefix, const std::string& srv, int id);
std::string light_bright_cmd_topic(const std::string& prefix, const std::string& srv, int id);
std::string cover_state_topic    (const std::string& prefix, const std::string& srv, const std::string& cover_id);
std::string cover_cmd_topic      (const std::string& prefix, const std::string& srv, const std::string& cover_id);
std::string sensor_state_topic   (const std::string& prefix, const std::string& srv, int id);

// ── HA MQTT discovery JSON builders ──────────────────────────────────────────

std::string make_switch_discovery(const std::string& prefix, const std::string& srv,
                                   const config::PcaConfig& e);
std::string make_switch_discovery(const std::string& prefix, const std::string& srv,
                                   const config::RelayConfig& e);
std::string make_light_discovery (const std::string& prefix, const std::string& srv,
                                   const config::PcaConfig& e);
std::string make_light_discovery (const std::string& prefix, const std::string& srv,
                                   const config::PwmConfig& e);
std::string make_cover_discovery (const std::string& prefix, const std::string& srv,
                                   const config::CoverConfig& c);
std::string make_sensor_discovery(const std::string& prefix, const std::string& srv,
                                   const config::SensorConfig& s);

// ── ServerBridge – manages one myIO server ────────────────────────────────────

class ServerBridge {
public:
    ServerBridge(config::ServerConfig                cfg,
                 std::string                         topic_prefix,
                 std::shared_ptr<ha::IHAPublisher>   publisher,
                 std::unique_ptr<myio::IMyIOClient>  client);

    // Publishes HA auto-discovery messages (called once at startup)
    net::awaitable<void> publish_discovery();

    // Periodically polls the server and publishes state changes
    net::awaitable<void> poll_loop();

    // Handles an incoming MQTT command; returns immediately if not for this server
    net::awaitable<void> handle_command(const std::string& topic,
                                         const std::string& payload);

    const std::string& server_id() const { return cfg_.server_id; }

private:
    // Returns the cover config that owns a given output id (or nullptr)
    const config::CoverConfig* find_cover_by_output(int output_id) const;

    // Returns the set of output ids that are part of a cover (not shown as standalone switches)
    std::unordered_set<int> cover_output_ids() const;

    // State publishers – only publish on change
    net::awaitable<void> publish_pca_state  (const myio::PcaOutput&   pca,
                                              const config::PcaConfig& cfg);
    net::awaitable<void> publish_relay_state(const myio::RelayOutput&   relay,
                                              const config::RelayConfig& cfg);
    net::awaitable<void> publish_cover_state(const config::CoverConfig& cover,
                                              int up_state, int down_state);
    net::awaitable<void> publish_sensor_state(const myio::Sensor&        sensor,
                                               const config::SensorConfig& cfg);

    config::ServerConfig                cfg_;
    std::string                         topic_prefix_;
    std::shared_ptr<ha::IHAPublisher>   publisher_;
    std::unique_ptr<myio::IMyIOClient>  client_;

    // Last known states for change detection
    std::unordered_map<int, int>    last_pca_state_;    // id → state (0–255); negative id = cover combined
    std::unordered_map<int, int>    last_relay_state_;  // id → state (0/1); negative id = cover combined
    std::unordered_map<int, double> last_sensor_val_;   // id → value
};

// ── Bridge – top-level coordinator for all servers ────────────────────────────

class Bridge {
public:
    Bridge(config::BridgeConfig              cfg,
           net::io_context&                  ioc,
           std::shared_ptr<ha::IHAPublisher> publisher);

    // Starts the full bridge: MQTT connect, discovery, poll loops, command loop.
    // Returns when the io_context stops.
    net::awaitable<void> run();

private:
    // Receives MQTT commands and dispatches to the appropriate ServerBridge
    net::awaitable<void> command_loop();

    config::BridgeConfig                       cfg_;
    net::io_context&                           ioc_;
    std::shared_ptr<ha::IHAPublisher>          publisher_;
    std::vector<std::unique_ptr<ServerBridge>> servers_;
};

} // namespace bridge
