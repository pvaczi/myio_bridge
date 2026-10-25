#include "bridge.hpp"
#include "config_reader.hpp"
#include "mqtt5_publisher.hpp"
#include <log.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "dev"
#endif

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::println("myIO bridge {}", BRIDGE_VERSION);
        return 0;
    }

    // Validates a config without touching the network; the Web UI runs it on
    // the temp file before the atomic swap, so one parser is the only judge.
    if (argc > 1 && std::string_view(argv[1]) == "--check-config") {
        const std::string path = (argc > 2) ? argv[2] : "bridge.json";
        try {
            const auto cfg = config::load_bridge_config(path);
            std::println("Config OK: {} server(s)", cfg.servers.size());
            return 0;
        } catch (const std::exception& ex) {
            std::println(stderr, "{}", ex.what());
            return 1;
        }
    }

    const std::string config_path = (argc > 1) ? argv[1] : "bridge.json";

    myio::log("[bridge] version {}", BRIDGE_VERSION);
    myio::log("[bridge] Loading config: {}", config_path);

    config::BridgeConfig cfg;
    try {
        cfg = config::load_bridge_config(config_path);
    } catch (const std::exception& ex) {
        myio::log(stderr, "[bridge] Config error: {}", ex.what());
        return 1;
    }

    // Status files go next to the config (bridge.json's directory; "." if bare name).
    cfg.data_dir = std::filesystem::absolute(std::filesystem::path(config_path)).parent_path();

    myio::log("[bridge] {} server(s) configured, MQTT: {}:{}",
              cfg.servers.size(), cfg.mqtt.host, cfg.mqtt.port);

    boost::asio::io_context ioc;

    auto publisher = std::make_shared<ha::MQTT5Publisher>(ioc, cfg.mqtt);
    publisher->set_will(bridge::bridge_availability_topic(cfg.mqtt.topic_prefix),
                        std::string(bridge::kAvailabilityOffline));
    bridge::Bridge br(cfg, ioc, publisher);

    // Handle Ctrl+C / SIGTERM gracefully: publish "offline" and disconnect,
    // but never wait longer than the guard for an unreachable broker.
    boost::asio::signal_set   signals(ioc, SIGINT, SIGTERM);
    boost::asio::steady_timer shutdown_guard(ioc);
    signals.async_wait([&](auto, auto) {
        std::println();
        myio::log("[bridge] Shutting down...");
        // Set before anything is aborted: run()'s completion must not report
        // the stop as a fatal error.
        br.request_stop();
        shutdown_guard.expires_after(std::chrono::seconds(3));
        shutdown_guard.async_wait([&ioc](auto) { ioc.stop(); });
        boost::asio::co_spawn(ioc, br.shutdown(),
            [&ioc](std::exception_ptr) { ioc.stop(); });
    });

    // run() never returns normally (its command loop is endless), so any
    // completion is fatal: stop the io_context and exit nonzero, otherwise the
    // poll/worker coroutines keep the process alive as a zombie and systemd
    // (Restart=on-failure) never restarts it. Exception: a requested stop
    // (SIGINT/SIGTERM) — the shutdown handler stops the io_context itself.
    int exit_code = 0;
    boost::asio::co_spawn(ioc,
        [&br]() -> boost::asio::awaitable<void> {
            co_await br.run();
        },
        [&ioc, &exit_code, &br](std::exception_ptr ep) {
            if (br.is_stopping()) return;
            if (ep) {
                try { std::rethrow_exception(ep); }
                catch (const std::exception& ex) {
                    myio::log(stderr, "[bridge] Fatal error: {}", myio::short_what(ex));
                } catch (...) {
                    myio::log(stderr, "[bridge] Fatal error: unknown exception");
                }
            } else {
                myio::log(stderr, "[bridge] Fatal error: main loop ended unexpectedly");
            }
            exit_code = 1;
            ioc.stop();
        });

    myio::log("[bridge] Running. Press Ctrl+C to stop.");
    ioc.run();
    myio::log("[bridge] Done.");
    return exit_code;
}
