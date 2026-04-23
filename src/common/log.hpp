#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <print>

namespace myio {

// ── Timestamp-prefixed logging ────────────────────────────────────────────────
// All log messages are prefixed with the current local date and time,
// truncated to second precision: [YYYY-MM-DD HH:MM:SS]
//
// Usage:
//   myio::log("[bridge] Starting up...");
//   myio::log(stderr, "[bridge] Error: {}", ex.what());

template<typename... Args>
void log(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::current_zone()->to_local(
            std::chrono::system_clock::now()));
    std::println(stream, "[{:%Y-%m-%d %H:%M:%S}] {}",
        now, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    log(stdout, fmt, std::forward<Args>(args)...);
}

} // namespace myio
