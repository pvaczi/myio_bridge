#pragma once

#include "IHAPublisher.hpp"
#include "config_types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/mqtt5/mqtt_client.hpp>

#include <string>
#include <string_view>
#include <tuple>

namespace net   = boost::asio;
namespace mqtt5 = boost::mqtt5;

namespace ha {

// ── Boost.MQTT5 publisher ─────────────────────────────────────────────────────
// Async, coroutine-friendly MQTT5 client.
// Boost.MQTT5 handles reconnection automatically.
class MQTT5Publisher final : public IHAPublisher {
public:
    MQTT5Publisher(net::io_context& ioc, config::MqttConfig cfg);
    ~MQTT5Publisher() override = default;

    MQTT5Publisher(const MQTT5Publisher&)            = delete;
    MQTT5Publisher& operator=(const MQTT5Publisher&) = delete;

    net::awaitable<void> async_connect()                                override;
    net::awaitable<void> async_publish(std::string_view topic,
                                        std::string_view payload,
                                        bool retain = true)             override;
    net::awaitable<void> async_subscribe(std::string_view topic_filter) override;
    [[nodiscard]]
    net::awaitable<std::tuple<std::string, std::string>> async_receive() override;
    net::awaitable<void> async_disconnect()                             override;

private:
    using client_t = mqtt5::mqtt_client<net::ip::tcp::socket>;

    config::MqttConfig cfg_;
    client_t           client_;
};

} // namespace ha
