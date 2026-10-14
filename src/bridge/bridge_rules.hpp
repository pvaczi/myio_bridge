#pragma once

// Pure, I/O-free rules of the bridge: HA component mapping of the configured
// outputs, strict MQTT command payload parsing and HTTP status-line parsing.
// Kept out of bridge.cpp so the unit tests can exercise them directly.

#include "config_types.hpp"

#include <optional>
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

} // namespace bridge
