#pragma once

#include <chrono>
#include <cstdio>
#include <exception>
#include <format>
#include <print>
#include <string>

namespace myio {

// Strip Boost internal source location from exception messages.
// Boost appends " [boost.beast:N at /path...]" or " [system:N at /path...]"
// which is useful when debugging Boost itself but is noise in production logs.
inline std::string short_what(const std::exception& ex) {
    std::string msg = ex.what();
    for (const char* tag : {" [boost", " [system:"}) {
        if (auto pos = msg.find(tag); pos != std::string::npos) {
            msg.resize(pos);
            break;
        }
    }
    return msg;
}

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
    std::fflush(stream);
}

template<typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    log(stdout, fmt, std::forward<Args>(args)...);
}

} // namespace myio
