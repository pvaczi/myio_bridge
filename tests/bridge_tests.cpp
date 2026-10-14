// Dependency-free unit tests for the bridge. Exit code is nonzero if any check fails.

#include "bridge_rules.hpp"
#include "config_reader.hpp"
#include "merge.hpp"

#include <boost/json.hpp>

#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

void test_cover_endpoint_rule() {
    using bridge::OutputKind;
    using bridge::find_enabled_cover;
    using bridge::is_cover_endpoint;

    auto cover = [](const char* id, bool enabled, const char* type, int up, int down) {
        config::CoverConfig c;
        c.cover_id = id;
        c.enabled  = enabled;
        c.up       = {type, up};
        c.down     = {type, down};
        return c;
    };
    const std::vector<config::CoverConfig> covers = {
        cover("pca_on",    true,  "pca",   3, 4),
        cover("pca_off",   false, "pca",   5, 6),
        cover("relay_on",  true,  "relay", 1, 2),
    };

    // Endpoints of an enabled cover, matched by output type and id
    CHECK(find_enabled_cover(covers, OutputKind::Pca, 3) == &covers[0]);
    CHECK(find_enabled_cover(covers, OutputKind::Pca, 4) == &covers[0]);
    CHECK(find_enabled_cover(covers, OutputKind::Relay, 2) == &covers[2]);
    CHECK(is_cover_endpoint(covers, OutputKind::Relay, 1));

    // Endpoints of a disabled cover are free outputs again
    CHECK(find_enabled_cover(covers, OutputKind::Pca, 5) == nullptr);
    CHECK(!is_cover_endpoint(covers, OutputKind::Pca, 6));

    // Same id in another output list is not the endpoint; PWM never is
    CHECK(!is_cover_endpoint(covers, OutputKind::Relay, 3));
    CHECK(!is_cover_endpoint(covers, OutputKind::Pca, 1));
    CHECK(!is_cover_endpoint(covers, OutputKind::Pwm, 3));
    CHECK(!is_cover_endpoint(covers, OutputKind::Pca, 99));
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

void test_percent_raw_conversion() {
    using bridge::percent_to_raw;
    using bridge::raw_to_percent;
    CHECK(percent_to_raw(0)   == 0);
    CHECK(percent_to_raw(100) == 255);
    CHECK(percent_to_raw(50)  == 128);  // round(127.5), half up
    CHECK(raw_to_percent(0)   == 0);
    CHECK(raw_to_percent(255) == 100);
    for (int p = 0; p <= 100; ++p) {
        CHECK(percent_to_raw(p) == static_cast<int>(std::lround(p * 255 / 100.0)));
        CHECK(raw_to_percent(percent_to_raw(p)) == p);
    }
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

void test_reserved_server_id() {
    CHECK(rejected_with(R"({"servers":[{"id":"bridge","host":"h"}]})", "reserved"));
}

void test_availability() {
    CHECK(bridge::bridge_availability_topic("myio") == "myio/bridge/availability");
    CHECK(bridge::server_availability_topic("myio", "lighting") == "myio/lighting/availability");

    const auto list = bridge::availability_list("myio", "lighting");
    CHECK(list.size() == 2);
    CHECK(list[0].as_object().at("topic").as_string() == "myio/bridge/availability");
    CHECK(list[1].as_object().at("topic").as_string() == "myio/lighting/availability");
}

void test_sensor_class() {
    using bridge::sensor_class;
    CHECK(sensor_class(1).device_class   == "temperature" && sensor_class(1).unit   == "°C");
    CHECK(sensor_class(100).device_class == "temperature");
    CHECK(sensor_class(101).device_class == "humidity"    && sensor_class(101).unit == "%");
    CHECK(sensor_class(108).device_class == "humidity");
    CHECK(sensor_class(201).device_class == "power"       && sensor_class(201).unit == "kW");
    CHECK(sensor_class(204).device_class == "voltage"     && sensor_class(206).unit == "V");
    CHECK(sensor_class(207).device_class == "current"     && sensor_class(209).unit == "A");
    // Impulse meter and unknown ids: numeric, but no device_class / unit.
    for (const int id : {109, 200, 210, 0}) {
        CHECK(sensor_class(id).device_class.empty());
        CHECK(sensor_class(id).unit.empty());
    }
    for (const int id : {1, 101, 200, 203, 205, 208, 500})
        CHECK(sensor_class(id).state_class == "measurement");
}

void test_discovery_blocks() {
    namespace json = boost::json;
    bridge::DiscoveryContext ctx;
    ctx.prefix         = "myio";
    ctx.server_id      = "lighting";
    ctx.host           = "10.0.0.5";
    ctx.port           = 80;
    ctx.bridge_version = "v1.5.0";

    // Unknown firmware: no sw_version; default port: no port in the URL.
    auto dev = bridge::server_device_block(ctx);
    CHECK(dev.at("identifiers").as_array().at(0).as_string() == "myio_lighting");
    CHECK(dev.at("name").as_string() == "myIO lighting");
    CHECK(!dev.contains("sw_version"));
    CHECK(dev.at("configuration_url").as_string() == "http://10.0.0.5/");
    CHECK(dev.at("via_device").as_string() == "myio_bridge");

    ctx.firmware_version = "3.9";
    ctx.port             = 8080;
    dev = bridge::server_device_block(ctx);
    CHECK(dev.at("sw_version").as_string() == "3.9");
    CHECK(dev.at("configuration_url").as_string() == "http://10.0.0.5:8080/");

    const auto origin = bridge::origin_block("v1.5.0");
    CHECK(origin.at("name").as_string() == "myio-bridge");
    CHECK(origin.at("sw_version").as_string() == "v1.5.0");

    json::object o;
    bridge::add_entity_common(o, ctx, "light", "myio_lighting_pca_2001");
    CHECK(o.at("default_entity_id").as_string() == "light.myio_lighting_pca_2001");
    CHECK(o.at("availability_mode").as_string() == "all");
    CHECK(o.at("availability").as_array().size() == 2);
    CHECK(o.at("device").as_object().at("via_device").as_string() == "myio_bridge");
    CHECK(o.at("origin").as_object().at("sw_version").as_string() == "v1.5.0");

    // Bridge device: registered via its connectivity entity; it must not depend
    // on its own availability, and its identifier is every via_device target.
    CHECK(bridge::bridge_status_discovery_topic() ==
          "homeassistant/binary_sensor/myio_bridge_connectivity/config");
    const auto bv = json::parse(bridge::make_bridge_status_discovery("myio", "v1.5.0")).as_object();
    CHECK(bv.at("state_topic").as_string() == "myio/bridge/availability");
    CHECK(bv.at("payload_on").as_string() == "online");
    CHECK(bv.at("payload_off").as_string() == "offline");
    CHECK(bv.at("device_class").as_string() == "connectivity");
    CHECK(bv.at("entity_category").as_string() == "diagnostic");
    CHECK(!bv.contains("availability"));
    CHECK(bv.at("device").as_object().at("identifiers").as_array().at(0).as_string() == "myio_bridge");
    CHECK(bv.at("device").as_object().at("sw_version").as_string() == "v1.5.0");
}

// ── discovery merge ───────────────────────────────────────────────────────────

namespace json = boost::json;

json::object obj(std::string_view text) { return json::parse(text).as_object(); }

const json::object* find_entry(const json::object& srv, const char* section, std::int64_t id) {
    const auto* arr = srv.if_contains(section);
    if (!arr) return nullptr;
    for (const auto& v : arr->as_array())
        if (v.as_object().at("id").to_number<std::int64_t>() == id) return &v.as_object();
    return nullptr;
}

void test_discovery_merge() {
    const auto existing = obj(R"({
        "id": "lighting", "host": "10.0.0.5", "port": 8080, "user": "u", "password": "secret",
        "poll_interval_sec": 9, "custom_server_key": [1, 2],
        "pca": [
            {"id": 2001, "name": "Old name", "type": "light", "enabled": true,
             "writable": false, "device_class": "outlet", "x_user": "keep"},
            {"id": 2002, "name": "Gone", "type": "switch", "enabled": true}
        ],
        "covers": [
            {"id": "cover_1", "name": "Blind", "enabled": true, "motor_timeout_sec": 42,
             "up": {"type": "relay", "id": 1, "_name": "old"}, "down": {"type": "relay", "id": 2}}
        ]
    })");
    const auto fresh = obj(R"({
        "id": "lighting", "host": "10.9.9.9", "port": 80, "user": "admin", "password": "admin",
        "poll_interval_sec": 5, "coalesce_window_ms": 5,
        "pca": [
            {"id": 2001, "name": "Dev name", "type": "switch", "enabled": false, "_pwm": true},
            {"id": 2003, "name": "New one", "type": "switch", "enabled": false}
        ],
        "covers": [
            {"id": "cover_1", "name": "Blind", "enabled": false, "motor_timeout_sec": 30,
             "up": {"type": "relay", "id": 1, "_name": "new"}, "down": {"type": "relay", "id": 2}}
        ]
    })");

    discovery::MergeReport report;
    const auto m = discovery::merge_server(&existing, fresh, report);

    // Server-level keys: existing wins, unknown key round-trips, gaps are filled
    CHECK(m.at("host").as_string() == "10.0.0.5");
    CHECK(m.at("port").to_number<int>() == 8080);
    CHECK(m.at("password").as_string() == "secret");
    CHECK(m.at("poll_interval_sec").to_number<int>() == 9);
    CHECK(m.at("custom_server_key").as_array().size() == 2);
    CHECK(m.at("coalesce_window_ms").to_number<int>() == 5);

    // Entry: user keys preserved, device facts refreshed
    const auto* e1 = find_entry(m, "pca", 2001);
    CHECK(e1 != nullptr);
    if (e1) {
        CHECK(e1->at("enabled").as_bool());
        CHECK(e1->at("type").as_string() == "light");
        CHECK(!e1->at("writable").as_bool());
        CHECK(e1->at("device_class").as_string() == "outlet");
        CHECK(e1->at("x_user").as_string() == "keep");
        CHECK(e1->at("name").as_string() == "Dev name");  // name is device-derived
        CHECK(e1->at("_pwm").as_bool());
    }

    // New output added with defaults
    const auto* e3 = find_entry(m, "pca", 2003);
    CHECK(e3 != nullptr && !e3->at("enabled").as_bool());

    // Removed output kept and reported
    const auto* e2 = find_entry(m, "pca", 2002);
    CHECK(e2 != nullptr && e2->at("enabled").as_bool());
    bool reported_gone = false, reported_new = false;
    for (const auto& line : report) {
        if (line.contains("2002") && line.contains("no longer")) reported_gone = true;
        if (line.contains("2003") && line.contains("new")) reported_new = true;
    }
    CHECK(reported_gone);
    CHECK(reported_new);

    // Cover: motor timeout and enabled preserved, nested diagnostics refreshed
    const auto& c = m.at("covers").as_array().at(0).as_object();
    CHECK(c.at("enabled").as_bool());
    CHECK(c.at("motor_timeout_sec").to_number<int>() == 42);
    CHECK(c.at("up").as_object().at("_name").as_string() == "new");

    // Section only in the existing file (device reports none now): entries kept
    const auto only_old = obj(R"({"id":"s","relays":[{"id":7,"name":"R","enabled":true}]})");
    const auto empty_fresh = obj(R"({"id":"s"})");
    discovery::MergeReport r2;
    const auto m2 = discovery::merge_server(&only_old, empty_fresh, r2);
    CHECK(find_entry(m2, "relays", 7) != nullptr);
    CHECK(r2.size() == 1);

    // No existing config: fresh entry as is
    discovery::MergeReport r3;
    CHECK(discovery::merge_server(nullptr, fresh, r3) == fresh);

    // Failing server: existing config unchanged, no _error stub; no existing config: stub
    discovery::MergeReport r4;
    const auto kept = discovery::failed_server(&existing, "lighting", "10.9.9.9", "timeout", r4);
    CHECK(kept == existing);
    CHECK(!kept.contains("_error"));
    CHECK(r4.size() == 1);
    const auto stub = discovery::failed_server(nullptr, "new", "h", "timeout", r4);
    CHECK(stub.at("_error").as_string() == "timeout");

    // find_server
    const auto root = obj(R"({"servers":[{"id":"a"},{"id":"b","host":"h"}]})");
    CHECK(discovery::find_server(root, "b") != nullptr);
    CHECK(discovery::find_server(root, "zz") == nullptr);
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
    test_cover_endpoint_rule();
    test_on_off_parsing();
    test_numeric_parsing();
    test_percent_raw_conversion();
    test_http_status_parsing();
    test_reserved_server_id();
    test_availability();
    test_sensor_class();
    test_discovery_blocks();
    test_discovery_merge();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
