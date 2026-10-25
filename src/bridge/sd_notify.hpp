#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// Minimal sd_notify(3) replacement: one datagram to $NOTIFY_SOCKET, no libsystemd.
namespace sdnotify {

// sun_path bytes for a NOTIFY_SOCKET value: "/path" as is, "@name" (abstract
// namespace) with the leading '@' turned into a NUL byte. Empty = unusable/unset.
std::string socket_address(const char* env_value);

// Watchdog ping period (half of WATCHDOG_USEC), or nullopt when the watchdog is
// not enabled for this process: unset/invalid/zero WATCHDOG_USEC, or a
// WATCHDOG_PID that is set but names another process.
std::optional<std::chrono::microseconds>
watchdog_period(const char* usec_env, const char* pid_env, long self_pid);

// Sends the state string (e.g. "READY=1") to $NOTIFY_SOCKET. No-op when it is
// unset and on Windows. Never throws; failures are ignored.
void notify(std::string_view state);

// Watchdog period for the running process, read from the environment.
std::optional<std::chrono::microseconds> watchdog_period_from_env();

} // namespace sdnotify
