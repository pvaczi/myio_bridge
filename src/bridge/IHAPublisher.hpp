#pragma once

#include <boost/asio/awaitable.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace net = boost::asio;

namespace ha {

// Thrown by async_receive when the client reconnected but the broker did not
// keep the session (e.g. broker restart): all subscriptions are gone and must
// be reinstated immediately — unlike a generic receive error, no backoff is
// needed, the connection itself is already up again.
struct SessionExpired : std::runtime_error {
    SessionExpired() : std::runtime_error("MQTT session expired on reconnect") {}
};

// ── Home Automation publisher interface (async) ───────────────────────────────
// Currently implemented with Boost.MQTT5.
// Can be replaced with a Matter implementation without changing the Bridge.
class IHAPublisher {
public:
    virtual ~IHAPublisher() = default;

    // Connect to the broker. Throws on failure.
    virtual net::awaitable<void> async_connect() = 0;

    // Publish a message.
    virtual net::awaitable<void> async_publish(std::string_view topic,
                                                std::string_view payload,
                                                bool retain = true) = 0;

    // Subscribe to a topic filter (wildcards '+' and '#' supported).
    virtual net::awaitable<void> async_subscribe(std::string_view topic_filter) = 0;

    // Wait for the next incoming message.
    // Returns: {topic, payload}
    // Throws if the connection is lost.
    [[nodiscard]]
    virtual net::awaitable<std::tuple<std::string, std::string>> async_receive() = 0;

    // Disconnect cleanly.
    virtual net::awaitable<void> async_disconnect() = 0;
};

} // namespace ha
