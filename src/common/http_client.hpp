#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace myio {

// ── Synchronous HTTP client (Boost.Beast) ─────────────────────────────────────
// Each call opens, uses, and closes an independent TCP connection.
// Sufficient for the discovery tool; the bridge uses the async variant.

struct HttpConfig {
    std::string          host;
    std::uint16_t        port     = 80;
    std::string          user     = "admin";
    std::string          password = "admin";
    std::string          server_id;               // used in error messages
    std::chrono::seconds timeout{10};
};

// Performs an HTTP GET and returns the JSON body (extracts { ... } from response).
// Throws on network or HTTP error.
[[nodiscard]]
std::string http_get(const HttpConfig& cfg, std::string_view target = "/");

// Performs an HTTP GET and returns the raw response body (everything after headers).
// Use this for non-JSON endpoints (e.g. XML).
[[nodiscard]]
std::string http_get_raw(const HttpConfig& cfg, std::string_view target);

} // namespace myio
