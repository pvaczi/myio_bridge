#pragma once

// Pure, I/O-free rules of the bridge: HA component mapping of the configured
// outputs, strict MQTT command payload parsing, HTTP status-line parsing,
// availability topics and the shared HA discovery blocks.
// Kept out of bridge.cpp so the unit tests can exercise them directly.

#include "config_types.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

// Strict ON/OFF: surrounding whitespace and letter case are ignored, anything
// else (including an empty payload, e.g. a retained-clear) → nullopt.
std::optional<bool> parse_on_off(std::string_view payload);

// Whole-payload decimal integer within [lo, hi] (surrounding whitespace allowed);
// empty, partial ("12abc"), fractional or out-of-range payloads → nullopt.
std::optional<int> parse_int_in_range(std::string_view payload, int lo, int hi);

enum class CoverCommand { Open, Close, Stop };

// OPEN / CLOSE / STOP (case-insensitive, whitespace-trimmed); anything else → nullopt.
std::optional<CoverCommand> parse_cover_command(std::string_view payload);

// Status code from the first line of a raw HTTP response ("HTTP/1.x NNN ...").
// Tolerates the myIO quirks: bare-LF line endings, missing or odd reason phrase.
// nullopt if there is no well-formed status line.
std::optional<int> parse_http_status(std::string_view raw);

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

} // namespace bridge
