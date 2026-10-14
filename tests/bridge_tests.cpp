// Dependency-free unit tests for the bridge. Exit code is nonzero if any check fails.

#include "config_reader.hpp"

#include <cstdio>
#include <exception>
#include <format>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// Returns the exception message, or "" if parsing succeeded.
std::string parse_error(std::string_view json) {
    try {
        (void)config::parse_bridge_config(json);
        return {};
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

bool rejected_with(std::string_view json, std::string_view needle) {
    const auto msg = parse_error(json);
    if (msg.empty()) return false;
    if (msg.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  unexpected message: %s\n", msg.c_str());
        return false;
    }
    return true;
}

// Builds a config with one server; the fragments are spliced in verbatim.
std::string make_config(std::string_view mqtt_extra = "",
                        std::string_view server_extra = "",
                        std::string_view covers = "[]") {
    return std::format(R"({{
        "mqtt": {{ "host": "broker" {} }},
        "servers": [ {{ "id": "lights", "host": "10.0.0.1" {}, "covers": {} }} ]
    }})", mqtt_extra, server_extra, covers);
}

constexpr std::string_view kValidCover = R"([{
    "id": "cover_1", "name": "Living", "enabled": true,
    "up":   { "type": "pca", "id": 3 },
    "down": { "type": "pca", "id": 4 }
}])";

void test_valid_config() {
    const auto cfg = config::parse_bridge_config(make_config("", "", kValidCover));
    CHECK(cfg.mqtt.topic_prefix == "myio");
    CHECK(cfg.servers.size() == 1);
    CHECK(cfg.servers[0].poll_interval_sec == 5);
    CHECK(cfg.servers[0].covers.size() == 1);
    CHECK(cfg.servers[0].covers[0].up.id == 3);
    CHECK(cfg.servers[0].covers[0].down.id == 4);
}

void test_poll_interval() {
    CHECK(rejected_with(make_config("", R"(, "poll_interval_sec": 0)"), "poll_interval_sec"));
    CHECK(rejected_with(make_config("", R"(, "poll_interval_sec": -3)"), "poll_interval_sec"));
    CHECK(parse_error(make_config("", R"(, "poll_interval_sec": 1)")).empty());
}

void test_topic_prefix() {
    CHECK(rejected_with(make_config(R"(, "topic_prefix": "")"), "topic_prefix"));
    CHECK(rejected_with(make_config(R"(, "topic_prefix": "my/io")"), "topic_prefix"));
    CHECK(rejected_with(make_config(R"(, "topic_prefix": "myio+")"), "topic_prefix"));
    CHECK(rejected_with(make_config(R"(, "topic_prefix": "#")"), "topic_prefix"));
    CHECK(parse_error(make_config(R"(, "topic_prefix": "home_myio")")).empty());
}

void test_server_id_and_ports() {
    CHECK(rejected_with(R"({"servers":[{"id":"a/b","host":"h"}]})", "Server id"));
    CHECK(rejected_with(make_config(R"(, "port": 0)"), "port"));
    CHECK(rejected_with(make_config(R"(, "port": 70000)"), "port"));
    CHECK(rejected_with(make_config("", R"(, "port": 65536)"), "port"));
}

void test_cover_endpoints() {
    // Missing endpoints — enabled and disabled alike.
    CHECK(rejected_with(make_config("", "", R"([{"id":"c1","enabled":true,
        "up":{"type":"pca","id":3}}])"), "'up' and 'down'"));
    CHECK(rejected_with(make_config("", "", R"([{"id":"c1","enabled":false,
        "down":{"type":"pca","id":4}}])"), "'up' and 'down'"));
    CHECK(rejected_with(make_config("", "", R"([{"id":"c1","enabled":true}])"),
                        "'up' and 'down'"));
    // Endpoint not an object.
    CHECK(rejected_with(make_config("", "", R"([{"id":"c1","up":3,"down":4}])"),
                        "'up' and 'down'"));
    // Unknown endpoint type.
    CHECK(rejected_with(make_config("", "", R"([{"id":"c1","enabled":true,
        "up":{"type":"Relay","id":3},"down":{"type":"relay","id":4}}])"), "pca"));
    // Cover id used as a topic level.
    CHECK(rejected_with(make_config("", "", R"([{"id":"c/1","enabled":true,
        "up":{"type":"pca","id":3},"down":{"type":"pca","id":4}}])"), "cover id"));
    // _comment entries are still skipped.
    CHECK(parse_error(make_config("", "", R"([{"_comment":"note"}])")).empty());
}

} // namespace

int main() {
    test_valid_config();
    test_poll_interval();
    test_topic_prefix();
    test_server_id_and_ports();
    test_cover_endpoints();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
