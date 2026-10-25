#pragma once

// Pure, I/O-free rules of the bridge: HA component mapping of the configured
// outputs, strict MQTT command payload parsing, HTTP status-line parsing,
// availability topics and the shared HA discovery blocks.
// Kept out of bridge.cpp so the unit tests can exercise them directly.

#include "config_types.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

// Which configuration list an output comes from.
enum class OutputKind { Pca, Relay, Pwm };

// HA component that owns the output's command topic when it is writable:
// "switch", "light" or "number"; "" = not individually commandable (cover endpoint).
std::string_view command_component(OutputKind kind, config::OutputType type);

// HA component the output is published as: the command component when writable,
// otherwise its read-only counterpart (switch → binary_sensor, light/number → sensor).
// "" = not published as a standalone entity (cover endpoint).
std::string_view ha_component(OutputKind kind, config::OutputType type, bool writable);

// ── Cover endpoints ───────────────────────────────────────────────────────────
// An output that is an endpoint of an ENABLED cover is driven only through its
// cover entity: discovery gives it no own entity, its own /set is dropped and
// the poll publishes it only as part of the cover state. Endpoints of a
// disabled cover are free outputs again. A PWM output is never an endpoint.

// The enabled cover whose up or down endpoint is output (kind, id); nullptr if none.
const config::CoverConfig* find_enabled_cover(const std::vector<config::CoverConfig>& covers,
                                              OutputKind kind, int id);

// find_enabled_cover(covers, kind, id) != nullptr
bool is_cover_endpoint(const std::vector<config::CoverConfig>& covers, OutputKind kind, int id);

// Strict ON/OFF: surrounding whitespace and letter case are ignored, anything
// else (including an empty payload, e.g. a retained-clear) → nullopt.
std::optional<bool> parse_on_off(std::string_view payload);

// Whole-payload decimal integer within [lo, hi] (surrounding whitespace allowed);
// empty, partial ("12abc"), fractional or out-of-range payloads → nullopt.
std::optional<int> parse_int_in_range(std::string_view payload, int lo, int hi);

// HA percentage (0–100) <-> myIO raw output value (0–255), both rounded to nearest.
// percent_to_raw → raw_to_percent round-trips exactly for every integer 0..100.
int percent_to_raw(int percent);
int raw_to_percent(int raw);

enum class CoverCommand { Open, Close, Stop };

// OPEN / CLOSE / STOP (case-insensitive, whitespace-trimmed); anything else → nullopt.
std::optional<CoverCommand> parse_cover_command(std::string_view payload);

// Status code from the first line of a raw HTTP response ("HTTP/1.x NNN ...").
// Tolerates the myIO quirks: bare-LF line endings, missing or odd reason phrase.
// nullopt if there is no well-formed status line.
std::optional<int> parse_http_status(std::string_view raw);

enum class PostVerdict { Accepted, Unauthorized, Rejected, NoStatus };

// Verdict on a myIO POST response from its parse_http_status() result.
// 2xx/3xx → Accepted (an embedded form handler may answer with a redirect);
// NoStatus (empty or status-less response) is not a definitive answer.
PostVerdict classify_post_status(std::optional<int> status);

// ── Availability ──────────────────────────────────────────────────────────────
// Retained "online"/"offline" topics. The bridge topic is also the MQTT Will
// (LWT); "bridge" is therefore a reserved server id (config_reader rejects it).

inline constexpr std::string_view kAvailabilityOnline  = "online";
inline constexpr std::string_view kAvailabilityOffline = "offline";

// "<prefix>/bridge/availability"
std::string bridge_availability_topic(std::string_view prefix);
// "<prefix>/<server_id>/availability"
std::string server_availability_topic(std::string_view prefix, std::string_view server_id);

// [{"topic": bridge}, {"topic": server}] — used with availability_mode "all",
// so an entity is available only while both the bridge and its device are.
boost::json::array availability_list(std::string_view prefix, std::string_view server_id);

// ── Sensor classification ─────────────────────────────────────────────────────
// HA metadata of a myIO sensor by its id range (see dsens_parser.cpp).
// Empty device_class / unit = none; state_class is always "measurement",
// because every published sensor value is numeric.
struct SensorClass {
    std::string_view device_class;
    std::string_view unit;
    std::string_view state_class;
};
SensorClass sensor_class(int id);

// ── Discovery common blocks ───────────────────────────────────────────────────

inline constexpr std::string_view kBridgeDeviceId = "myio_bridge";

// Per-server inputs of the shared discovery blocks.
struct DiscoveryContext {
    std::string   prefix;            // MQTT topic prefix
    std::string   server_id;
    std::string   host;
    std::uint16_t port = 80;
    std::string   firmware_version;  // from /info.json; "" = unknown (omitted)
    std::string   bridge_version;
};

// {"name": "myio-bridge", "sw_version": <bridge version>}
boost::json::object origin_block(std::string_view bridge_version);

// The myIO server's HA device: unchanged identifiers/name, plus sw_version
// (when known), configuration_url and via_device = the bridge device.
boost::json::object server_device_block(const DiscoveryContext& ctx);

// The bridge's own HA device (target of every server's via_device).
boost::json::object bridge_device_block(std::string_view bridge_version);

// Adds device, origin, availability (+ mode "all") and
// default_entity_id = "<component>.<object_id>" to an entity's discovery config.
void add_entity_common(boost::json::object& o, const DiscoveryContext& ctx,
                       std::string_view component, std::string_view object_id);

// Bridge connectivity binary_sensor (diagnostic) — registers the bridge device.
std::string bridge_status_discovery_topic();
std::string make_bridge_status_discovery(std::string_view prefix,
                                         std::string_view bridge_version);

// Bridge-level HA buttons (config category). Pressing one publishes kButtonPress
// on "<prefix>/bridge/<action>" — under the reserved "bridge" level, so no
// server ever claims the command. Both act only on the MQTT side: no extra
// request reaches the myIO devices.
enum class BridgeButton { Rediscover, Resync };
inline constexpr BridgeButton kBridgeButtons[] = { BridgeButton::Rediscover, BridgeButton::Resync };
inline constexpr std::string_view kButtonPress = "PRESS";

std::string bridge_button_command_topic(std::string_view prefix, BridgeButton b);
std::string bridge_button_discovery_topic(BridgeButton b);
std::string make_bridge_button_discovery(std::string_view prefix,
                                         std::string_view bridge_version, BridgeButton b);
// The button a received command addresses; nullopt = not a bridge button press.
std::optional<BridgeButton> bridge_button_pressed(std::string_view prefix,
                                                  std::string_view topic,
                                                  std::string_view payload);

// ── Diagnostics ───────────────────────────────────────────────────────────────
// Per-server poll diagnostics (HA diagnostic sensors) and the bridge start time.
// Built purely from what the existing poll already measured — no extra request
// ever reaches the myIO device.

// "YYYY-MM-DDTHH:MM:SSZ" (UTC, whole seconds).
std::string format_iso_utc(std::chrono::system_clock::time_point t);

// Inputs of the per-server diag JSON; nullopt serializes as JSON null.
struct ServerDiag {
    std::optional<std::chrono::system_clock::time_point> last_poll_ok;
    std::optional<int> poll_latency_ms;     // last successful async_poll() call
    int         consecutive_errors = 0;
    long long   total_errors       = 0;     // failed polls since bridge start
    std::string last_error;                 // most recent poll error, kept after recovery
    std::string firmware;                   // "" = unknown
    std::string info_error;                 // "" once an /info.json fetch succeeded
};

// "<prefix>/<server_id>/diag" (retained JSON state of the server diagnostics)
std::string server_diag_topic(std::string_view prefix, std::string_view server_id);
std::string make_server_diag_state(const ServerDiag& d);
// Content of <data dir>/status/<server_id>.json (read by the Web UI): the diag
// fields plus "online" (null = not yet determined) and "written_at".
std::string make_server_status_state(const ServerDiag& d, std::optional<bool> online,
                                     std::chrono::system_clock::time_point now);

enum class DiagEntity { LastPoll, PollLatency, ConsecutiveErrors, TotalErrors,
                        LastError, Firmware, InfoError };
inline constexpr DiagEntity kServerDiagEntities[] = {
    DiagEntity::LastPoll, DiagEntity::PollLatency, DiagEntity::ConsecutiveErrors,
    DiagEntity::TotalErrors, DiagEntity::LastError, DiagEntity::Firmware, DiagEntity::InfoError };

// "homeassistant/sensor/myio_<server>_diag_<key>/config"
std::string server_diag_discovery_topic(std::string_view server_id, DiagEntity e);
// Diagnostic sensor on the server device. Available on the BRIDGE topic only, not
// the server's: the diagnostics must stay visible while the server is offline.
std::string make_server_diag_discovery(const DiscoveryContext& ctx, DiagEntity e);

// "<prefix>/bridge/started" (retained ISO-8601 UTC start time of the bridge process)
std::string bridge_started_topic(std::string_view prefix);
std::string bridge_started_discovery_topic();
std::string make_bridge_started_discovery(std::string_view prefix,
                                          std::string_view bridge_version);

} // namespace bridge
