#include "mqtt5_publisher.hpp"
#include <log.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mqtt5/error.hpp>
#include <boost/mqtt5/types.hpp>

#include <format>
#include <stdexcept>
#include <string>
#include <tuple>

namespace ha {

// ── Constructor ───────────────────────────────────────────────────────────────

MQTT5Publisher::MQTT5Publisher(net::io_context& ioc, config::MqttConfig cfg)
    : cfg_(std::move(cfg))
    , client_(ioc.get_executor())
{}

void MQTT5Publisher::set_will(std::string topic, std::string payload) {
    will_topic_   = std::move(topic);
    will_payload_ = std::move(payload);
}

// ── Connect ───────────────────────────────────────────────────────────────────

net::awaitable<void> MQTT5Publisher::async_connect() {
    client_.brokers(cfg_.host, cfg_.port);

    if (!cfg_.user.empty())
        client_.credentials(cfg_.client_id, cfg_.user, cfg_.password);

    // Sent in every CONNECT, so it also covers the automatic reconnects.
    if (!will_topic_.empty())
        client_.will(mqtt5::will{will_topic_, will_payload_,
                                 mqtt5::qos_e::at_least_once, mqtt5::retain_e::yes});

    // async_run maintains the connection and handles automatic reconnection.
    // It does not block the caller.
    client_.async_run(net::detached);

    myio::log("[mqtt] Connecting to {}:{}", cfg_.host, cfg_.port);
    co_return;
}

// ── Publish ───────────────────────────────────────────────────────────────────

net::awaitable<void> MQTT5Publisher::async_publish(std::string_view topic,
                                                     std::string_view payload,
                                                     bool retain) {
    const auto retain_flag = retain ? mqtt5::retain_e::yes : mqtt5::retain_e::no;

    if (retain) {
        // QoS 1 returns (error_code, puback_reason_code, puback_props)
        auto [ec, reason_code, props] =
            co_await client_.async_publish<mqtt5::qos_e::at_least_once>(
                std::string(topic),
                std::string(payload),
                retain_flag,
                mqtt5::publish_props{},
                net::as_tuple(net::use_awaitable));

        // Propagate instead of logging: callers update their change-detection
        // cache only after a successful publish, so the value is retried later.
        if (ec)
            throw std::runtime_error(
                std::format("[mqtt] Publish error ({}): {}", topic, ec.message()));
        if (reason_code)  // operator bool: true = error reason code (>= 0x80)
            throw std::runtime_error(
                std::format("[mqtt] PUBACK error ({}): {}", topic, reason_code.message()));
    } else {
        // QoS 0 – fire and forget, no acknowledgement
        co_await client_.async_publish<mqtt5::qos_e::at_most_once>(
            std::string(topic),
            std::string(payload),
            retain_flag,
            mqtt5::publish_props{},
            net::use_awaitable);
    }
}

// ── Subscribe ─────────────────────────────────────────────────────────────────

net::awaitable<void> MQTT5Publisher::async_subscribe(std::string_view topic_filter) {
    std::vector<mqtt5::subscribe_topic> subs;
    subs.push_back({
        std::string(topic_filter),
        mqtt5::subscribe_options{
            mqtt5::qos_e::at_least_once,
            // Without this, the broker echoes the bridge's own retained state
            // publishes back to itself (same client ID) on this same "myio/#"
            // subscription — every state publish then takes a pointless round
            // trip through enqueue_if_mine/command_worker before prepare_command
            // discards it as a non-"/set" topic.
            mqtt5::no_local_e::yes,
            mqtt5::retain_as_published_e::retain,
            mqtt5::retain_handling_e::not_send
        }
    });

    // async_subscribe returns (error_code, vector<reason_code>, suback_props)
    auto [ec, reason_codes, props] =
        co_await client_.async_subscribe(
            std::move(subs),
            mqtt5::subscribe_props{},
            net::as_tuple(net::use_awaitable));

    if (ec)
        throw std::runtime_error(
            std::format("[mqtt] Subscribe error ({}): {}", topic_filter, ec.message()));
    // A refused filter (e.g. not authorized) is reported per topic in the
    // SUBACK, not via ec — without this check the bridge would be silently deaf.
    for (const auto& rc : reason_codes)
        if (rc)  // error reason code (>= 0x80)
            throw std::runtime_error(
                std::format("[mqtt] SUBACK error ({}): {}", topic_filter, rc.message()));

    myio::log("[mqtt] Subscribed: {}", topic_filter);
}

// ── Receive ───────────────────────────────────────────────────────────────────

net::awaitable<std::tuple<std::string, std::string>> MQTT5Publisher::async_receive() {
    // async_receive returns (error_code, topic, payload, publish_props)
    auto [ec, topic, payload, props] =
        co_await client_.async_receive(net::as_tuple(net::use_awaitable));

    // The client reconnected but the broker lost the session (broker restart):
    // every subscription is gone. Signal it distinctly so the command loop can
    // re-subscribe immediately instead of applying the generic error backoff.
    if (ec == mqtt5::client::error::session_expired)
        throw SessionExpired{};

    if (ec)
        throw std::runtime_error(
            std::format("[mqtt] Receive error: {}", ec.message()));

    co_return std::make_tuple(std::move(topic), std::move(payload));
}

// ── Disconnect ────────────────────────────────────────────────────────────────

net::awaitable<void> MQTT5Publisher::async_disconnect() {
    // async_disconnect returns (error_code)
    auto [ec] =
        co_await client_.async_disconnect(
            mqtt5::disconnect_rc_e::normal_disconnection,
            mqtt5::disconnect_props{},
            net::as_tuple(net::use_awaitable));

    if (ec)
        myio::log(stderr, "[mqtt] Disconnect error: {}", ec.message());

    myio::log("[mqtt] Disconnected.");
    co_return;
}

} // namespace ha
