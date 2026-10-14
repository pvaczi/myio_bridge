// Dependency-free unit tests for the bridge. Exit code is nonzero if any check fails.

#include "bridge_rules.hpp"
#include "config_reader.hpp"

#include <cstdio>
#include <exception>
#include <format>
#include <optional>
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

void test_writable_and_device_class() {
    const auto cfg = config::parse_bridge_config(make_config("", R"(,
        "pca": [
            { "id": 2001, "type": "switch", "enabled": true },
            { "id": 2002, "type": "sensor_output", "enabled": true, "writable": false,
              "device_class": "heat" },
            { "id": 2003, "type": "light", "enabled": true, "writable": true }
        ],
        "relays": [ { "id": 5, "enabled": true, "writable": false } ],
        "pwm":    [ { "id": 101, "type": "number", "enabled": true, "writable": false } ])"));
    const auto& srv = cfg.servers[0];
    CHECK(srv.pca.size() == 3);
    CHECK(srv.pca[0].writable);                 // default: existing configs unchanged
    CHECK(srv.pca[0].device_class.empty());     // default: no device_class
    CHECK(!srv.pca[1].writable);
    CHECK(srv.pca[1].device_class == "heat");
    CHECK(srv.pca[2].writable);
    CHECK(srv.relays.size() == 1 && !srv.relays[0].writable);
    CHECK(srv.pwm.size() == 1 && !srv.pwm[0].writable);

    // Wrong value types are rejected, not silently defaulted.
    CHECK(rejected_with(make_config("", R"(, "pca": [{"id":2001,"writable":"no"}])"), "writable"));
    CHECK(rejected_with(make_config("", R"(, "pca": [{"id":2001,"device_class":""}])"),
                        "device_class"));
    CHECK(rejected_with(make_config("", R"(, "relays": [{"id":1,"device_class":3}])"),
                        "device_class"));
}

void test_cover_not_read_only() {
    CHECK(rejected_with(make_config("", "", R"([{
        "id": "c1", "enabled": true, "writable": false,
        "up": {"type":"pca","id":3}, "down": {"type":"pca","id":4}}])"), "cover"));
    CHECK(rejected_with(make_config("", R"(, "pca": [
        {"id":2001,"type":"cover_up","writable":false}])"), "cover endpoint"));
    CHECK(rejected_with(make_config("", R"(, "relays": [
        {"id":2,"type":"cover_down","writable":false}])"), "cover endpoint"));
    // writable: true is the default and stays valid on covers.
    CHECK(parse_error(make_config("", "", R"([{
        "id": "c1", "enabled": true, "writable": true,
        "up": {"type":"pca","id":3}, "down": {"type":"pca","id":4}}])")).empty());
}

void test_component_mapping() {
    using bridge::OutputKind;
    using config::OutputType;
    using bridge::ha_component;
    using bridge::command_component;

    CHECK(ha_component(OutputKind::Pca, OutputType::Switch,       true)  == "switch");
    CHECK(ha_component(OutputKind::Pca, OutputType::SensorOutput, true)  == "switch");
    CHECK(ha_component(OutputKind::Pca, OutputType::Light,        true)  == "light");
    CHECK(ha_component(OutputKind::Pca, OutputType::Number,       true)  == "number");
    CHECK(ha_component(OutputKind::Pca, OutputType::Switch,       false) == "binary_sensor");
    CHECK(ha_component(OutputKind::Pca, OutputType::SensorOutput, false) == "binary_sensor");
    CHECK(ha_component(OutputKind::Pca, OutputType::Light,        false) == "sensor");
    CHECK(ha_component(OutputKind::Pca, OutputType::Number,       false) == "sensor");
    CHECK(ha_component(OutputKind::Pca, OutputType::CoverUp,      true)  == "");
    CHECK(command_component(OutputKind::Pca, OutputType::CoverDown).empty());

    CHECK(ha_component(OutputKind::Relay, OutputType::Switch, true)  == "switch");
    CHECK(ha_component(OutputKind::Relay, OutputType::Light,  true)  == "switch");
    CHECK(ha_component(OutputKind::Relay, OutputType::Switch, false) == "binary_sensor");
    CHECK(command_component(OutputKind::Relay, OutputType::CoverUp).empty());

    CHECK(ha_component(OutputKind::Pwm, OutputType::Light,  true)  == "light");
    CHECK(ha_component(OutputKind::Pwm, OutputType::Number, true)  == "number");
    CHECK(ha_component(OutputKind::Pwm, OutputType::Light,  false) == "sensor");
}

void test_on_off_parsing() {
    CHECK(bridge::parse_on_off("ON")    == std::optional<bool>(true));
    CHECK(bridge::parse_on_off("OFF")   == std::optional<bool>(false));
    CHECK(bridge::parse_on_off(" on\n") == std::optional<bool>(true));
    // Empty payload = retained-clear: must never act as OFF.
    CHECK(!bridge::parse_on_off(""));
    CHECK(!bridge::parse_on_off("   "));
    CHECK(!bridge::parse_on_off("1"));
    CHECK(!bridge::parse_on_off("0"));
    CHECK(!bridge::parse_on_off("TOGGLE"));
    CHECK(!bridge::parse_on_off("ONN"));
    CHECK(!bridge::parse_on_off(R"({"state":"ON"})"));
}

void test_numeric_parsing() {
    using bridge::parse_int_in_range;
    CHECK(parse_int_in_range("0", 0, 100)     == std::optional<int>(0));
    CHECK(parse_int_in_range("100", 0, 100)   == std::optional<int>(100));
    CHECK(parse_int_in_range(" 42 ", 0, 100)  == std::optional<int>(42));
    CHECK(parse_int_in_range("255", 0, 255)   == std::optional<int>(255));
    CHECK(!parse_int_in_range("", 0, 100));
    CHECK(!parse_int_in_range("101", 0, 100));
    CHECK(!parse_int_in_range("-1", 0, 100));
    CHECK(!parse_int_in_range("256", 0, 255));
    CHECK(!parse_int_in_range("12abc", 0, 100));
    CHECK(!parse_int_in_range("12.5", 0, 100));
    CHECK(!parse_int_in_range("ON", 0, 100));
    CHECK(!parse_int_in_range("99999999999999999999", 0, 100));

    CHECK(bridge::parse_cover_command("OPEN")  == std::optional(bridge::CoverCommand::Open));
    CHECK(bridge::parse_cover_command("close") == std::optional(bridge::CoverCommand::Close));
    CHECK(bridge::parse_cover_command("STOP")  == std::optional(bridge::CoverCommand::Stop));
    CHECK(!bridge::parse_cover_command(""));
    CHECK(!bridge::parse_cover_command("50"));
}

void test_http_status_parsing() {
    using bridge::parse_http_status;
    CHECK(parse_http_status("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") == std::optional<int>(200));
    CHECK(parse_http_status("HTTP/1.0 200 OK\nContent-Type: text/html\n\n<html>") == std::optional<int>(200));
    CHECK(parse_http_status("HTTP/1.1 401 Unauthorized\r\n\r\n") == std::optional<int>(401));
    CHECK(parse_http_status("HTTP/1.1 503\r\n\r\n") == std::optional<int>(503));
    CHECK(parse_http_status("HTTP/1.1  204 \n") == std::optional<int>(204));
    CHECK(parse_http_status("HTTP/1.1 302") == std::optional<int>(302));
    CHECK(!parse_http_status(""));
    CHECK(!parse_http_status("<html>401</html>"));
    CHECK(!parse_http_status("HTTP/1.1\r\n"));
    CHECK(!parse_http_status("HTTP/1.1 20\r\n"));
    CHECK(!parse_http_status("HTTP/1.1 2000 OK\r\n"));
    // Status must come from the status line, not from a later header.
    CHECK(!parse_http_status("garbage\r\nContent-Length: 200\r\n"));
}

} // namespace

int main() {
    test_valid_config();
    test_poll_interval();
    test_topic_prefix();
    test_server_id_and_ports();
    test_cover_endpoints();
    test_writable_and_device_class();
    test_cover_not_read_only();
    test_component_mapping();
    test_on_off_parsing();
    test_numeric_parsing();
    test_http_status_parsing();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
