#pragma once

#include "IMyIOClient.hpp"
#include <http_client.hpp>
#include <types.hpp>

#include <boost/asio/awaitable.hpp>
#include <string>
#include <string_view>

namespace net = boost::asio;

namespace myio {

// ── Asynchronous HTTP client (Boost.Beast) ────────────────────────────────────
// Uses Boost.Asio awaitables so it runs without blocking inside the Bridge's
// io_context. Bypasses Beast's HTTP parser because the myIO server sends
// non-standard HTTP headers.
class AsyncHttpClient final : public IMyIOClient {
public:
    explicit AsyncHttpClient(HttpConfig cfg);

    // GET /info.json → returns server metadata (called once at startup)
    [[nodiscard]]
    net::awaitable<ServerInfo> async_get_info() override;

    // GET /sens_out.json → parses and returns current state
    [[nodiscard]]
    net::awaitable<ServerState> async_poll() override;

    // POST / – sends command string to myIO
    // Example body: "PCA_ON=2005" or "PCA_OFF=2022&PCA_ON=2023"
    net::awaitable<void> async_send_command(std::string post_body) override;

private:
    [[nodiscard]]
    net::awaitable<std::string> do_get(std::string_view target);

    net::awaitable<void> do_post(std::string_view target,
                                  std::string_view body);

    HttpConfig cfg_;
};

} // namespace myio
