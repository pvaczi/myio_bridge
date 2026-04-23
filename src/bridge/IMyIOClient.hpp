#pragma once

#include <types.hpp>

#include <boost/asio/awaitable.hpp>
#include <string>

namespace net = boost::asio;

namespace myio {

// ── myIO server communication interface (async) ───────────────────────────────
// The implementation (currently Boost.Beast HTTP) is swappable.
// The Bridge only communicates with myIO through this interface.
class IMyIOClient {
public:
    virtual ~IMyIOClient() = default;

    // Fetches server info (info.json) — called once at startup.
    [[nodiscard]]
    virtual net::awaitable<ServerInfo> async_get_info() = 0;

    // Polls the server for current state (sens_out.json).
    [[nodiscard]]
    virtual net::awaitable<ServerState> async_poll() = 0;

    // Sends an HTTP POST command to the server.
    // Example: "PCA_ON=2005", "r_OFF=3", "PCA_OFF=2022&PCA_ON=2023"
    virtual net::awaitable<void> async_send_command(std::string post_body) = 0;
};

} // namespace myio
