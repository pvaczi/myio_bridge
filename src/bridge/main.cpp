#include "bridge.hpp"
#include "config_reader.hpp"
#include "mqtt5_publisher.hpp"
#include <log.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

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

    myio::log("[bridge] {} server(s) configured, MQTT: {}:{}",
              cfg.servers.size(), cfg.mqtt.host, cfg.mqtt.port);

    boost::asio::io_context ioc;

    auto publisher = std::make_shared<ha::MQTT5Publisher>(ioc, cfg.mqtt);
    bridge::Bridge br(cfg, ioc, publisher);

    // Handle Ctrl+C / SIGTERM gracefully
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
        std::println();
        myio::log("[bridge] Shutting down...");
        ioc.stop();
    });

    // run() never returns normally (its command loop is endless), so any
    // completion is fatal: stop the io_context and exit nonzero, otherwise the
    // poll/worker coroutines keep the process alive as a zombie and systemd
    // (Restart=on-failure) never restarts it.
    int exit_code = 0;
    boost::asio::co_spawn(ioc,
        [&br]() -> boost::asio::awaitable<void> {
            co_await br.run();
        },
        [&ioc, &exit_code](std::exception_ptr ep) {
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
