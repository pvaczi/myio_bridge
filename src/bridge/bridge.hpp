#pragma once

#include "bridge_rules.hpp"
#include "config_types.hpp"
#include "IHAPublisher.hpp"
#include "IMyIOClient.hpp"
#include <types.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
std::string cover_position_topic (const std::string& prefix, const std::string& srv, const std::string& cover_id);
std::string cover_cmd_topic      (const std::string& prefix, const std::string& srv, const std::string& cover_id);
std::string sensor_state_topic   (const std::string& prefix, const std::string& srv, int id);
std::string number_state_topic   (const std::string& prefix, const std::string& srv, int id);
// Read-only (writable: false) light: brightness in % for its HA sensor
std::string light_percent_topic  (const std::string& prefix, const std::string& srv, int id);

// ── HA MQTT discovery JSON builders ──────────────────────────────────────────
// ctx carries the topic prefix, server id and the shared device/origin inputs.

std::string make_switch_discovery(const DiscoveryContext& ctx, const config::PcaConfig& e);
std::string make_switch_discovery(const DiscoveryContext& ctx, const config::RelayConfig& e);
std::string make_light_discovery (const DiscoveryContext& ctx, const config::PcaConfig& e,
                                   bool brightness);
std::string make_light_discovery (const DiscoveryContext& ctx, const config::PwmConfig& e);
std::string make_number_discovery(const DiscoveryContext& ctx, const config::PcaConfig& e);
std::string make_number_discovery(const DiscoveryContext& ctx, const config::PwmConfig& e);
std::string make_cover_discovery (const DiscoveryContext& ctx, const config::CoverConfig& c);
std::string make_sensor_discovery(const DiscoveryContext& ctx, const config::SensorConfig& s);

// ── ServerBridge – manages one myIO server ────────────────────────────────────

// A built-but-not-yet-sent myIO command: the POST fragment plus the follow-up
// that publishes optimistic state to the broker after the POST succeeds.
// Splitting build from send lets command_worker coalesce several commands into
// a single HTTP POST (one TCP round-trip instead of one per command).
struct PreparedCommand {
    std::string                           post;          // "" = nothing to send
    std::function<net::awaitable<void>()> publish_state; // run after the POST
};

class ServerBridge {
public:
    ServerBridge(config::ServerConfig                cfg,
                 std::string                         topic_prefix,
                 std::shared_ptr<ha::IHAPublisher>   publisher,
                 std::unique_ptr<myio::IMyIOClient>  client,
                 const net::any_io_executor&         ex);

    // Publishes HA auto-discovery messages (called once at startup)
    net::awaitable<void> publish_discovery();

    // Periodically polls the server and publishes state changes
    net::awaitable<void> poll_loop();

    // Per-server command worker: drains the queue, coalesces commands that arrive
    // within a short window into a single POST, then publishes state. Spawned once.
    net::awaitable<void> command_worker();

    // Enqueues a command for processing by command_worker; no-op if the topic
    // targets another server.
    net::awaitable<void> enqueue_if_mine(const std::string& topic,
                                         const std::string& payload);

    // Resets last-known states so the next poll republishes everything to the broker.
    // Call after broker reconnect to restore retained state topics.
    void schedule_resync();

    const std::string& server_id() const { return cfg_.server_id; }

private:
    using cmd_channel_t = net::experimental::concurrent_channel<
        void(boost::system::error_code, std::string, std::string)>;

    // Builds a command's POST fragment + state-publish follow-up without sending.
    // Returns nullopt if the topic is not a known command for this server.
    std::optional<PreparedCommand> prepare_command(const std::string& topic,
                                                    const std::string& payload);

    // Sends one coalesced POST for the batch, then runs each follow-up in order.
    net::awaitable<void> process_command_batch(
        const std::vector<std::pair<std::string, std::string>>& batch);

    // State publishers – only publish on change
    net::awaitable<void> publish_pca_state  (const myio::PcaOutput&   pca,
                                              const config::PcaConfig& cfg);
    net::awaitable<void> publish_relay_state(const myio::RelayOutput&   relay,
                                              const config::RelayConfig& cfg);
    net::awaitable<void> publish_pwm_state  (const myio::PwmOutput&   pwm,
                                              const config::PwmConfig& cfg);
    net::awaitable<void> publish_cover_state(const config::CoverConfig& cover,
                                              int up_state, int down_state, int dir_hint);
    net::awaitable<void> publish_sensor_state(const myio::Sensor&        sensor,
                                               const config::SensorConfig& cfg,
                                               bool                        do_log = true);

    bool should_log_now() const { return !syncing_ || cfg_.log_startup_sync; }

    // Prefix, server id, host and the known firmware/bridge versions for the
    // discovery builders.
    DiscoveryContext discovery_context() const;

    // Discovery config of an enabled, writable PCA output for its component
    // (light dimmability taken from pca_is_pwm_).
    std::string pca_discovery_payload(const config::PcaConfig& e,
                                      std::string_view component) const;

    // GET /info.json: logs the server info and stores firmware_version_.
    // Returns false (error in last_info_error_) on failure.
    net::awaitable<bool> fetch_server_info();

    // Records the pwm flags of a poll; re-sends the light discovery of every
    // enabled writable PCA light whose dimmability became known or changed.
    net::awaitable<void> update_pca_capabilities(const myio::ServerState& state);

    // Publishes the server availability (retained) when it differs from the
    // last successfully published value (or nothing was published yet).
    net::awaitable<void> update_availability(bool online);

    config::ServerConfig                cfg_;
    std::string                         topic_prefix_;
    std::shared_ptr<ha::IHAPublisher>   publisher_;
    std::unique_ptr<myio::IMyIOClient>  client_;
    cmd_channel_t                       cmd_chan_;   // HA→myIO command queue
    // Parallel FIFO of enqueue timestamps, one per item currently sitting in
    // cmd_chan_. Single producer (enqueue_if_mine) / single consumer
    // (command_worker), pushed/popped in lockstep with the channel, so it stays
    // aligned without needing to change the channel's completion signature.
    // Lets command_worker log how long after the first command each coalesced
    // sibling actually arrived — data to size the coalesce window from, instead
    // of guessing.
    std::deque<std::chrono::steady_clock::time_point> cmd_arrival_times_;

    // Last known states for change detection
    std::unordered_map<int, int>    last_pca_state_;    // id → state (0–255); negative id = cover combined
    std::unordered_map<int, int>    last_relay_state_;  // id → state (0/1); negative id = cover combined
    std::unordered_map<int, int>    last_pwm_state_;    // id → state (0–255)
    std::unordered_map<int, double> last_sensor_val_;        // id → last published value
    std::unordered_map<int, double> last_sensor_logged_val_; // id → last logged value
    std::unordered_map<int, int>                    last_cover_dir_;     // -(up.id) → +1=up, -1=down
    std::unordered_map<int, std::shared_ptr<bool>> last_cover_cancel_;  // -(up.id) → motor timeout cancel flag

    // Hardware capability cache: filled by the discovery's capability poll and
    // kept up to date from every regular poll (a failed first poll must not
    // leave dimmable lights non-dimmable).
    std::unordered_map<int, bool>   pca_is_pwm_;        // id → pwm flag

    // From /info.json, fetched before the first discovery, else by poll_loop;
    // "" until a fetch succeeded.
    std::string firmware_version_;
    std::string last_info_error_;
    // Set when poll_loop starts: from then on only the poll loop talks HTTP to
    // the device (discovery must not run requests in parallel with it).
    bool poll_started_ = false;
    // Last successfully published server availability; nullopt = republish.
    std::optional<bool> published_available_;
    // Covers whose retained state was seeded ("stopped"/50) on the first discovery.
    std::unordered_set<std::string> seeded_covers_;

    std::string last_poll_error_;
    bool        syncing_ = false;   // true during startup/resync poll — tags log lines as [startup sync]
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

    // Clean shutdown: publishes the bridge "offline" availability (the Will is
    // not sent on a normal disconnect), then disconnects from the broker.
    net::awaitable<void> shutdown();

    // True once a stop was requested (signal handler / shutdown()): errors
    // caused by the shutdown itself (aborted MQTT operations) are not failures.
    bool is_stopping() const { return stopping_; }
    void request_stop() { stopping_ = true; }

private:
    // Receives MQTT commands and dispatches to the appropriate ServerBridge
    net::awaitable<void> command_loop();

    // Publishes the bridge's own discovery (connectivity entity) and then every
    // server's discovery concurrently. The bridge device goes first: it is the
    // via_device of every server device.
    net::awaitable<void> publish_all_discovery();

    // Builds one no-arg coroutine per server that (re)publishes its discovery
    // config, so callers can fan them out concurrently instead of one at a time.
    std::vector<std::function<net::awaitable<void>()>> discovery_tasks();

    // Retained "online" on the bridge availability topic; errors are logged.
    net::awaitable<void> publish_bridge_online();

    // Re-sends discovery after `delay` in a detached coroutine, so command
    // processing is not blocked; requests that arrive while one is pending are
    // coalesced into it.
    void schedule_discovery_resend(std::chrono::milliseconds delay);

    // HA birth message: re-send after a random 1–10 s delay.
    void schedule_birth_discovery();

    // A bridge button press from HA (see BridgeButton).
    void handle_bridge_button(BridgeButton b);

    config::BridgeConfig                       cfg_;
    net::io_context&                           ioc_;
    std::shared_ptr<ha::IHAPublisher>          publisher_;
    std::vector<std::unique_ptr<ServerBridge>> servers_;
    bool                                       discovery_resend_pending_ = false;
    bool                                       stopping_ = false;
};

} // namespace bridge
