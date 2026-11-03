// Dependency-free unit tests for the bridge. Exit code is nonzero if any check fails.

#include "bridge_rules.hpp"
#include "config_reader.hpp"
#include "dsens_parser.hpp"
#include "merge.hpp"
#include "sd_notify.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
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

void test_coalesce_window() {
    CHECK(rejected_with(make_config("", R"(, "coalesce_window_ms": -1)"), "coalesce_window_ms"));
    CHECK(rejected_with(make_config("", R"(, "coalesce_window_ms": 10001)"), "coalesce_window_ms"));
    CHECK(parse_error(make_config("", R"(, "coalesce_window_ms": 0)")).empty());
}

void test_log_debug() {
    CHECK(!config::parse_bridge_config(make_config()).servers[0].log_debug);
    CHECK(config::parse_bridge_config(make_config("", R"(, "log_debug": true)")).servers[0].log_debug);
    CHECK(rejected_with(make_config("", R"(, "log_debug": "yes")"), "log_debug"));
}

void test_required_sections() {
    CHECK(rejected_with(R"({"mqtt":{"host":"b"},"servers":[]})", "servers"));
    CHECK(rejected_with(R"({"mqtt":{"host":"b"}})", "servers"));
    CHECK(rejected_with(R"({"servers":[{"id":"a","host":"h"}]})", "mqtt"));
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

void test_post_verdict() {
    using bridge::classify_post_status;
    using bridge::PostVerdict;
    CHECK(classify_post_status(200) == PostVerdict::Accepted);
    CHECK(classify_post_status(204) == PostVerdict::Accepted);
    CHECK(classify_post_status(302) == PostVerdict::Accepted);
    CHECK(classify_post_status(401) == PostVerdict::Unauthorized);
    CHECK(classify_post_status(101) == PostVerdict::Rejected);
    CHECK(classify_post_status(404) == PostVerdict::Rejected);
    CHECK(classify_post_status(503) == PostVerdict::Rejected);
    CHECK(classify_post_status(std::nullopt) == PostVerdict::NoStatus);
    // The bare-LF device answer (measured) still counts as accepted.
    CHECK(classify_post_status(bridge::parse_http_status(
              "HTTP/1.1 200 OK\nContent-Type: text/html\n\n<html>")) == PostVerdict::Accepted);
    CHECK(classify_post_status(bridge::parse_http_status("")) == PostVerdict::NoStatus);
}

void test_post_chunking() {
    using bridge::chunk_post_fragments;
    using V = std::vector<std::string>;
    CHECK(chunk_post_fragments({}).empty());
    CHECK(chunk_post_fragments({"a=1", "b=2"}) == (V{"a=1&b=2"}));
    CHECK(chunk_post_fragments({"", "a=1"}) == (V{"a=1"}));
    // Exact fit (3 + 1 + 3 = 7) stays together, one over splits.
    CHECK(chunk_post_fragments({"aaa", "bbb"}, 7) == (V{"aaa&bbb"}));
    CHECK(chunk_post_fragments({"aaa", "bbb"}, 6) == (V{"aaa", "bbb"}));
    // A fragment longer than the limit goes alone and is never split.
    const std::string big(30, 'x');
    CHECK(chunk_post_fragments({"a=1", big, "b=2"}, 10) == (V{"a=1", big, "b=2"}));

    // 30 fragments of 12 chars: every chunk <= 200, nothing lost or reordered.
    V frags;
    for (int i = 0; i < 30; ++i) frags.push_back(std::format("PCA*{}=191", 2001 + i));
    const auto chunks = chunk_post_fragments(frags);
    CHECK(chunks.size() > 1);
    std::string rejoined;
    bool all_fit = true;
    for (const auto& c : chunks) {
        if (c.size() > 200) all_fit = false;
        rejoined += (rejoined.empty() ? "" : "&") + c;
    }
    CHECK(all_fit);
    std::string expected;
    for (const auto& f : frags) expected += (expected.empty() ? "" : "&") + f;
    CHECK(rejoined == expected);
}

void test_extract_json_body() {
    using bridge::extract_json_body;
    CHECK(extract_json_body("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"a\":1}", "h") == "{\"a\":1}");
    CHECK(extract_json_body("HTTP/1.1 200 OK\nContent-Type: application/json\n\n{\"a\":1}", "h") == "{\"a\":1}");

    auto throws_with = [](const std::string& raw, const char* needle) {
        try { (void)extract_json_body(raw, "h"); }
        catch (const std::exception& e) {
            return std::string_view(e.what()).find(needle) != std::string_view::npos;
        }
        return false;
    };
    // The real 401 reply: complete header block, HTML body without JSON.
    const std::string crlf401 =
        "HTTP/1.1 401 Authorization Required\r\nWWW-Authenticate: Basic realm=\"Secure Area\"\r\n"
        "Content-Type: text/html\r\nConnnection: close\r\n\r\n<html><body>401</body></html>";
    CHECK(throws_with(crlf401, "401"));
    std::string lf401;
    for (const char c : crlf401) if (c != '\r') lf401 += c;
    CHECK(throws_with(lf401, "401"));
    CHECK(throws_with("HTTP/1.1 200 OK\r\n\r\n<html></html>", "No JSON"));
    CHECK(throws_with("HTTP/1.1 200 OK\r\nX: y", "header terminator"));
}

void test_unreadable_outputs_skipped() {
    // {"id":N} without "state" = the configured user cannot read that output.
    const auto st = myio::DSensOutParser::parse(
        R"({"relays":{"0":{"id":1,"state":1},"1":{"id":2}},)"
        R"("PCA":{"0":{"id":2001,"state":50},"1":{"id":2002}},)"
        R"("PWM":{"0":{"id":101,"state":0},"1":{"id":102}}})", "t");
    CHECK(st.relay_outputs.size() == 1 && st.relay_outputs[0].id == 1);
    CHECK(st.pca_outputs.size() == 1 && st.pca_outputs[0].id == 2001);
    CHECK(st.pwm_outputs.size() == 1 && st.pwm_outputs[0].id == 101);
}

void test_sensor_activator_classification() {
    // sensor: 0 = none, 255 = SUN, 1..254 = sensor id. The lighting device has
    // 255 is an ordinary (manually switchable) output: not sensor-controlled.
    const auto st = myio::DSensOutParser::parse(
        R"({"PCA":{"0":{"id":2001,"state":0,"sensor":0},"1":{"id":2002,"state":0,"sensor":7},)"
        R"("2":{"id":2003,"state":0,"sensor":255}}})", "t");
    CHECK(st.pca_outputs.size() == 3);
    CHECK(!st.pca_outputs[0].sensor_controlled);
    CHECK(st.pca_outputs[1].sensor_controlled);
    CHECK(!st.pca_outputs[2].sensor_controlled && st.pca_outputs[2].sensor_id == 255);
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

    // Bridge buttons: on the bridge device, available with the bridge only.
    using bridge::BridgeButton;
    CHECK(bridge::bridge_button_discovery_topic(BridgeButton::Rediscover) ==
          "homeassistant/button/myio_bridge_rediscover/config");
    const auto rb = json::parse(bridge::make_bridge_button_discovery(
        "myio", "v1.5.0", BridgeButton::Resync)).as_object();
    CHECK(rb.at("command_topic").as_string() == "myio/bridge/resync");
    CHECK(rb.at("payload_press").as_string() == "PRESS");
    CHECK(rb.at("availability_topic").as_string() == "myio/bridge/availability");
    CHECK(rb.at("entity_category").as_string() == "config");
    CHECK(rb.at("unique_id").as_string() == "myio_bridge_resync");
    CHECK(rb.at("device").as_object().at("identifiers").as_array().at(0).as_string() == "myio_bridge");

    CHECK(bridge::bridge_button_pressed("myio", "myio/bridge/rediscover", "PRESS") ==
          BridgeButton::Rediscover);
    CHECK(bridge::bridge_button_pressed("myio", "myio/bridge/resync", "PRESS") ==
          BridgeButton::Resync);
    CHECK(!bridge::bridge_button_pressed("myio", "myio/bridge/resync", "ON"));
    CHECK(!bridge::bridge_button_pressed("myio", "other/bridge/resync", "PRESS"));
    CHECK(!bridge::bridge_button_pressed("myio", "myio/bridge/availability", "PRESS"));
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

void test_mqtt_status() {
    namespace json = boost::json;
    using namespace std::chrono;
    const auto t0 = sys_days{year{2026} / 11 / 3} + hours{8};

    // Fresh process: offline, only the start time known, every other field null.
    bridge::MqttStatus s;
    s.broker = "10.0.0.2:1883";
    s.since  = t0;
    auto o = json::parse(bridge::make_mqtt_status_state(s, t0 + seconds{5})).as_object();
    CHECK(o.at("connected").as_bool() == false);
    CHECK(o.at("broker").as_string() == "10.0.0.2:1883");
    CHECK(o.at("since").as_string() == "2026-11-03T08:00:00Z");
    CHECK(o.at("last_connect").is_null());
    CHECK(o.at("last_disconnect").is_null());
    CHECK(o.at("reconnects").as_int64() == 0);
    CHECK(o.at("last_error").is_null());
    CHECK(o.at("written_at").as_string() == "2026-11-03T08:00:05Z");

    // A failure while still offline records the error but keeps `since`.
    bridge::mqtt_status_disconnected(s, t0 + minutes{1}, "refused");
    CHECK(s.since == t0);
    CHECK(!s.last_disconnect);
    CHECK(s.last_error == "refused");

    // First connect: not a reconnect.
    bridge::mqtt_status_connected(s, t0 + minutes{2});
    o = json::parse(bridge::make_mqtt_status_state(s, t0 + minutes{3})).as_object();
    CHECK(o.at("connected").as_bool() == true);
    CHECK(o.at("since").as_string() == "2026-11-03T08:02:00Z");
    CHECK(o.at("last_connect").as_string() == "2026-11-03T08:02:00Z");
    CHECK(o.at("reconnects").as_int64() == 0);
    CHECK(o.at("last_error").as_string() == "refused");   // kept after recovery

    // Connection lost, then re-established: one reconnect.
    bridge::mqtt_status_disconnected(s, t0 + minutes{10}, "connection reset");
    o = json::parse(bridge::make_mqtt_status_state(s, t0 + minutes{10})).as_object();
    CHECK(o.at("connected").as_bool() == false);
    CHECK(o.at("since").as_string() == "2026-11-03T08:10:00Z");
    CHECK(o.at("last_disconnect").as_string() == "2026-11-03T08:10:00Z");
    CHECK(o.at("last_error").as_string() == "connection reset");
    bridge::mqtt_status_connected(s, t0 + minutes{11});
    CHECK(s.reconnects == 1);

    // An error-less disconnect (clean shutdown) keeps the previous last_error.
    bridge::mqtt_status_disconnected(s, t0 + minutes{12}, "");
    CHECK(!s.connected);
    CHECK(s.last_error == "connection reset");

    // The payload never carries credentials.
    const auto raw = bridge::make_mqtt_status_state(s, t0);
    CHECK(raw.find("user") == std::string::npos);
    CHECK(raw.find("password") == std::string::npos);
}

void test_diagnostics() {
    namespace json = boost::json;
    using namespace std::chrono;

    // Timestamp: whole seconds, UTC, trailing Z.
    CHECK(bridge::format_iso_utc(sys_days{year{2026} / 10 / 14} + hours{8} + minutes{5} + seconds{9} +
                                 milliseconds{750}) == "2026-10-14T08:05:09Z");

    // Nothing measured yet: nullable fields are JSON null.
    bridge::ServerDiag d;
    auto s = json::parse(bridge::make_server_diag_state(d)).as_object();
    CHECK(s.at("last_poll_ok").is_null());
    CHECK(s.at("poll_latency_ms").is_null());
    CHECK(s.at("consecutive_errors").as_int64() == 0);
    CHECK(s.at("total_errors").as_int64() == 0);
    CHECK(s.at("last_error").as_string() == "");
    CHECK(s.at("firmware").as_string() == "");
    CHECK(s.at("info_error").as_string() == "");

    d.last_poll_ok       = sys_days{year{2026} / 1 / 2} + hours{3};
    d.poll_latency_ms    = 412;
    d.consecutive_errors = 2;
    d.total_errors       = 7;
    d.last_error         = "timeout";
    d.firmware           = "3.9";
    d.info_error         = "refused";
    s = json::parse(bridge::make_server_diag_state(d)).as_object();
    CHECK(s.at("last_poll_ok").as_string() == "2026-01-02T03:00:00Z");
    CHECK(s.at("poll_latency_ms").as_int64() == 412);
    CHECK(s.at("consecutive_errors").as_int64() == 2);
    CHECK(s.at("total_errors").as_int64() == 7);
    CHECK(s.at("last_error").as_string() == "timeout");
    CHECK(s.at("firmware").as_string() == "3.9");
    CHECK(s.at("info_error").as_string() == "refused");

    CHECK(bridge::server_diag_topic("myio", "lighting") == "myio/lighting/diag");

    // Status file content: diag fields + online (null/true/false) + written_at.
    const auto now = sys_days{year{2026} / 10 / 14} + hours{8};
    auto st = json::parse(bridge::make_server_status_state(d, std::nullopt, now)).as_object();
    CHECK(st.at("online").is_null());
    CHECK(st.at("written_at").as_string() == "2026-10-14T08:00:00Z");
    CHECK(st.at("last_poll_ok").as_string() == "2026-01-02T03:00:00Z");
    CHECK(st.at("consecutive_errors").as_int64() == 2);
    CHECK(st.at("firmware").as_string() == "3.9");
    st = json::parse(bridge::make_server_status_state(d, true, now)).as_object();
    CHECK(st.at("online").as_bool() == true);
    st = json::parse(bridge::make_server_status_state(bridge::ServerDiag{}, false, now)).as_object();
    CHECK(st.at("online").as_bool() == false);
    CHECK(st.at("last_poll_ok").is_null());
    CHECK(!st.contains("slot"));   // no slot known -> no "slot" key

    // Slot object: active + boot; boot omitted when bootUpSlot is unknown.
    bridge::SlotInfo si;
    si.index = 3; si.name = "Esti"; si.boot_index = 1; si.boot_name = "Alap";
    st = json::parse(bridge::make_server_status_state(d, true, now, si)).as_object();
    const auto& sl = st.at("slot").as_object();
    CHECK(sl.at("active").at("index").as_int64() == 3 && sl.at("active").at("name").as_string() == "Esti");
    CHECK(sl.at("boot").at("index").as_int64() == 1 && sl.at("boot").at("name").as_string() == "Alap");
    si.boot_index.reset(); si.boot_name.clear();
    st = json::parse(bridge::make_server_status_state(d, true, now, si)).as_object();
    CHECK(st.at("slot").as_object().contains("active") && !st.at("slot").as_object().contains("boot"));
    CHECK(st.at("online").as_bool() == true && st.at("written_at").as_string() == "2026-10-14T08:00:00Z");

    bridge::DiscoveryContext ctx;
    ctx.prefix         = "myio";
    ctx.server_id      = "lighting";
    ctx.host           = "10.0.0.5";
    ctx.bridge_version = "v1.7.0";

    // Every server diag entity: diagnostic, bridge availability ONLY (visible
    // while the server is offline), the diag state topic, own unique_id/topic.
    std::vector<std::string> uids;
    for (const auto e : bridge::kServerDiagEntities) {
        const auto o = json::parse(bridge::make_server_diag_discovery(ctx, e)).as_object();
        CHECK(o.at("entity_category").as_string() == "diagnostic");
        CHECK(o.at("availability_topic").as_string() == "myio/bridge/availability");
        CHECK(!o.contains("availability"));
        CHECK(!o.contains("availability_mode"));
        CHECK(o.at("state_topic").as_string() == "myio/lighting/diag");
        CHECK(o.at("device").as_object().at("identifiers").as_array().at(0).as_string() == "myio_lighting");
        CHECK(o.at("origin").as_object().at("sw_version").as_string() == "v1.7.0");
        const std::string uid = std::string(o.at("unique_id").as_string());
        CHECK(uid.starts_with("myio_lighting_diag_"));
        CHECK(o.at("default_entity_id").as_string() == "sensor." + uid);
        CHECK(bridge::server_diag_discovery_topic("lighting", e) ==
              "homeassistant/sensor/" + uid + "/config");
        uids.push_back(uid);
    }
    std::ranges::sort(uids);
    CHECK(std::ranges::adjacent_find(uids) == uids.end());   // all distinct
    CHECK(uids.size() == 7);

    const auto lat = json::parse(
        bridge::make_server_diag_discovery(ctx, bridge::DiagEntity::PollLatency)).as_object();
    CHECK(lat.at("unique_id").as_string() == "myio_lighting_diag_poll_latency");
    CHECK(lat.at("device_class").as_string() == "duration");
    CHECK(lat.at("unit_of_measurement").as_string() == "ms");
    CHECK(lat.at("state_class").as_string() == "measurement");
    const auto lp = json::parse(
        bridge::make_server_diag_discovery(ctx, bridge::DiagEntity::LastPoll)).as_object();
    CHECK(lp.at("device_class").as_string() == "timestamp");
    CHECK(!lp.contains("unit_of_measurement"));
    const auto te = json::parse(
        bridge::make_server_diag_discovery(ctx, bridge::DiagEntity::TotalErrors)).as_object();
    CHECK(te.at("state_class").as_string() == "total_increasing");

    // Bridge start time sensor.
    CHECK(bridge::bridge_started_topic("myio") == "myio/bridge/started");
    CHECK(bridge::bridge_started_discovery_topic() == "homeassistant/sensor/myio_bridge_started/config");
    const auto bs = json::parse(bridge::make_bridge_started_discovery("myio", "v1.7.0")).as_object();
    CHECK(bs.at("unique_id").as_string() == "myio_bridge_started");
    CHECK(bs.at("default_entity_id").as_string() == "sensor.myio_bridge_started");
    CHECK(bs.at("state_topic").as_string() == "myio/bridge/started");
    CHECK(bs.at("availability_topic").as_string() == "myio/bridge/availability");
    CHECK(bs.at("device_class").as_string() == "timestamp");
    CHECK(bs.at("entity_category").as_string() == "diagnostic");
    CHECK(bs.at("device").as_object().at("identifiers").as_array().at(0).as_string() == "myio_bridge");
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

    // Successful run over an earlier failure stub: the stale `_error` is dropped
    discovery::MergeReport r5;
    const auto recovered = discovery::merge_server(&stub, obj(R"({"id":"new","host":"h","port":80})"), r5);
    CHECK(!recovered.contains("_error"));
    CHECK(recovered.at("port").to_number<int>() == 80);

    // find_server
    const auto root = obj(R"({"servers":[{"id":"a"},{"id":"b","host":"h"}]})");
    CHECK(discovery::find_server(root, "b") != nullptr);
    CHECK(discovery::find_server(root, "zz") == nullptr);
}

void test_sd_notify() {
    using std::chrono::microseconds;
    CHECK(sdnotify::socket_address(nullptr).empty());
    CHECK(sdnotify::socket_address("").empty());
    CHECK(sdnotify::socket_address("relative/path").empty());
    CHECK(sdnotify::socket_address("/run/systemd/notify") == "/run/systemd/notify");
    CHECK(sdnotify::socket_address("@abstract") == std::string("\0abstract", 9));

    CHECK(sdnotify::watchdog_period("60000000", nullptr, 42) == microseconds{30000000});
    CHECK(sdnotify::watchdog_period("60000000", "", 42) == microseconds{30000000});
    CHECK(sdnotify::watchdog_period("60000000", "42", 42) == microseconds{30000000});
    CHECK(!sdnotify::watchdog_period("60000000", "43", 42));
    CHECK(!sdnotify::watchdog_period(nullptr, nullptr, 42));
    CHECK(!sdnotify::watchdog_period("", nullptr, 42));
    CHECK(!sdnotify::watchdog_period("0", nullptr, 42));
    CHECK(!sdnotify::watchdog_period("-5", nullptr, 42));
    CHECK(!sdnotify::watchdog_period("12abc", nullptr, 42));
    CHECK(!sdnotify::watchdog_period("60000000", "x", 42));
}

// Trimmed copy of the heating controller's GET /output page (fw 3.7.5.1): the six
// arrays of the real page, the PCA ones listed first and between other JS.
constexpr const char* kOutputPage =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
    "<html><script>\n"
    " var other=[1,2,3];\n"
    " var PCA_thermoActivator=[0,0,0,0,0,0,0,0,0,55,45,53,2,62,36,37,1,40,38,39,60,58,59,63,0,0,0,0,0,0,0,0];\n"
    " var PCA_min_temp_ON=[0,0,0,0,0,0,0,0,0,197,216,225,244,244,244,244,244,223,244,244,225,225,218,218,150,0,0,0,0,0,0,0];\n"
    " var PCA_max_temp_OFF=[0,0,0,0,0,0,0,0,0,203,220,229,246,246,246,246,246,227,246,246,229,229,222,222,148,0,0,0,0,0,0,0];\n"
    " var thermoActivator=[53,40,43,45,54,0,61,0,57,49,0,0,0,0,0,0];\n"
    " var min_temp_ON=[229,227,222,220,227,227,227,227,227,150,0,0,0,0,0,0];\n"
    " var max_temp_OFF=[225,223,218,216,223,223,223,223,223,148,0,0,0,0,0,0];\n"
    "</script></html>\n";

void test_parse_js_int_array() {
    using bridge::parse_js_int_array;
    const std::string_view page = kOutputPage;

    const auto relay_on = parse_js_int_array(page, "min_temp_ON");
    CHECK(relay_on && relay_on->size() == 16 && (*relay_on)[0] == 229 && (*relay_on)[9] == 150);
    const auto pca_on = parse_js_int_array(page, "PCA_min_temp_ON");
    CHECK(pca_on && pca_on->size() == 32 && (*pca_on)[9] == 197 && (*pca_on)[24] == 150);

    // The unprefixed name must not match the PCA_ one, whichever comes first.
    const std::string_view only_pca = "var PCA_min_temp_ON=[1,2,3];";
    CHECK(!parse_js_int_array(only_pca, "min_temp_ON"));
    CHECK(parse_js_int_array(only_pca, "PCA_min_temp_ON") == std::vector<int>({1, 2, 3}));
    const auto act = parse_js_int_array(page, "thermoActivator");
    CHECK(act && act->size() == 16 && (*act)[0] == 53 && (*act)[6] == 61);
    const auto pca_act = parse_js_int_array(page, "PCA_thermoActivator");
    CHECK(pca_act && pca_act->size() == 32 && (*pca_act)[9] == 55);

    // Whitespace tolerant, negative numbers, empty array
    CHECK(parse_js_int_array("var  a \t=\n[ 1 , -2,3 ] ;", "a") == std::vector<int>({1, -2, 3}));
    CHECK(parse_js_int_array("x;\nvar a=[];", "a") == std::vector<int>{});

    // Not a `var` declaration, or a longer identifier
    CHECK(!parse_js_int_array("a=[1,2];", "a"));
    CHECK(!parse_js_int_array("let a=[1,2];", "a"));
    CHECK(!parse_js_int_array("xvar a=[1,2];", "a"));
    CHECK(!parse_js_int_array("var a2=[1,2];", "a"));
    CHECK(!parse_js_int_array("var a=5;", "a"));
    CHECK(!parse_js_int_array(page, "no_such_array"));
    CHECK(!parse_js_int_array("", "a"));
    CHECK(!parse_js_int_array(page, ""));

    // Malformed array bodies
    CHECK(!parse_js_int_array("var a=[1,x,3];", "a"));
    CHECK(!parse_js_int_array("var a=[1,2", "a"));
    CHECK(!parse_js_int_array("var a=[1,2,];", "a"));
    CHECK(!parse_js_int_array("var a=[1 2];", "a"));
    CHECK(!parse_js_int_array("var a=[", "a"));
}

void test_thermostat_mapping() {
    using bridge::OutputKind;
    using bridge::map_thermostat_thresholds;

    const std::string page = kOutputPage;
    const auto m = map_thermostat_thresholds(page, {1, 2, 6, 7, 8, 10, 11, 17, 0, -3}, {2001, 2010, 2017, 2033, 2000});
    CHECK(m.has_value());
    if (!m) return;

    auto find = [&](OutputKind kind, int id) -> const bridge::ThermostatThreshold* {
        for (const auto& t : *m) if (t.kind == kind && t.id == id) return &t;
        return nullptr;
    };
    // Relay id N → array index N-1: id 1 → index 0 (activator 53, 229/225)
    const auto* r1 = find(OutputKind::Relay, 1);
    CHECK(r1 && r1->activator == 53 && r1->on_raw == 229 && r1->off_raw == 225);
    const auto* r7 = find(OutputKind::Relay, 7);
    CHECK(r7 && r7->activator == 61 && r7->on_raw == 227 && r7->off_raw == 223);
    const auto* r10 = find(OutputKind::Relay, 10);
    CHECK(r10 && r10->activator == 49 && r10->on_raw == 150 && r10->off_raw == 148);
    // activator 0 (id 6 → index 5, id 8 → index 7, id 11 → index 10) skipped
    CHECK(!find(OutputKind::Relay, 6) && !find(OutputKind::Relay, 8) && !find(OutputKind::Relay, 11));
    // out of range / non-positive ids skipped
    CHECK(!find(OutputKind::Relay, 17) && !find(OutputKind::Relay, 0) && !find(OutputKind::Relay, -3));

    // PCA id 2010 → index 9: activator 55, 197 / 203
    const auto* p10 = find(OutputKind::Pca, 2010);
    CHECK(p10 && p10->activator == 55 && p10->on_raw == 197 && p10->off_raw == 203);
    const auto* p17 = find(OutputKind::Pca, 2017);   // index 16: activator 1, the lowest sensor id
    CHECK(p17 && p17->activator == 1 && p17->on_raw == 244 && p17->off_raw == 246);
    CHECK(!find(OutputKind::Pca, 2001));             // activator 0
    CHECK(!find(OutputKind::Pca, 2033) && !find(OutputKind::Pca, 2000));
    CHECK(m->size() == 6);                           // relays 1, 2, 7, 10 + PCA 2010, 2017
}

void test_thermostat_mapping_edges() {
    using bridge::OutputKind;
    using bridge::map_thermostat_thresholds;

    // Humidity activators (>= 100) are not temperature thresholds; 99 is still one.
    const std::string page =
        "var thermoActivator=[100,99,150,0];var min_temp_ON=[1,2,3,4];var max_temp_OFF=[5,6,7,8];";
    const auto m = map_thermostat_thresholds(page, {1, 2, 3, 4}, {});
    CHECK(m && m->size() == 1);
    if (m && m->size() == 1) {
        CHECK((*m)[0].id == 2 && (*m)[0].activator == 99 && (*m)[0].on_raw == 2 && (*m)[0].off_raw == 6);
    }

    // Only one group present: the other is skipped, not an error
    const auto pca_only = map_thermostat_thresholds(
        "var PCA_thermoActivator=[7];var PCA_min_temp_ON=[10];var PCA_max_temp_OFF=[20];",
        {1}, {2001});
    CHECK(pca_only && pca_only->size() == 1 && (*pca_only)[0].kind == OutputKind::Pca);

    // Arrays of different lengths: the index must exist in all three
    const auto uneven = map_thermostat_thresholds(
        "var thermoActivator=[5,5];var min_temp_ON=[1,2];var max_temp_OFF=[3];", {1, 2}, {});
    CHECK(uneven && uneven->size() == 1 && (*uneven)[0].id == 1);

    // A page with the arrays but nothing configured / nothing matching: parsed, empty
    const auto none = map_thermostat_thresholds(kOutputPage, {}, {});
    CHECK(none && none->empty());

    // Missing arrays: not a thermostat page; a triplet with one array missing counts as missing
    CHECK(!map_thermostat_thresholds("<html>no arrays</html>", {1}, {2001}));
    CHECK(!map_thermostat_thresholds("", {1}, {2001}));
    CHECK(!map_thermostat_thresholds("var thermoActivator=[5];var min_temp_ON=[1];", {1}, {2001}));
    // Malformed array: same as missing
    CHECK(!map_thermostat_thresholds(
        "var thermoActivator=[5,;var min_temp_ON=[1];var max_temp_OFF=[2];", {1}, {}));
}

void test_threshold_entities() {
    using bridge::OutputKind;
    using bridge::ThresholdKind;
    namespace json = boost::json;

    CHECK(bridge::format_tenths(229) == "22.9");
    CHECK(bridge::format_tenths(150) == "15.0");
    CHECK(bridge::format_tenths(0) == "0.0");
    CHECK(bridge::format_tenths(7) == "0.7");
    CHECK(bridge::format_tenths(-5) == "-0.5");
    CHECK(bridge::format_tenths(-123) == "-12.3");

    CHECK(bridge::threshold_state_topic("myio", "heating", OutputKind::Pca, 2010, ThresholdKind::On)
          == "myio/heating/threshold/pca/2010/on");
    CHECK(bridge::threshold_state_topic("myio", "heating", OutputKind::Relay, 3, ThresholdKind::Off)
          == "myio/heating/threshold/relay/3/off");
    CHECK(bridge::threshold_discovery_topic("heating", OutputKind::Pca, 2010, ThresholdKind::Off)
          == "homeassistant/sensor/myio_heating_pca_2010_thr_off/config");

    bridge::DiscoveryContext ctx;
    ctx.prefix = "myio"; ctx.server_id = "heating"; ctx.host = "10.0.0.5"; ctx.bridge_version = "v1";
    const auto on = json::parse(bridge::make_threshold_discovery(
        ctx, OutputKind::Pca, 2010, "Nappali", ThresholdKind::On)).as_object();
    CHECK(on.at("name").as_string() == "Nappali bekapcsolási küszöb");
    CHECK(on.at("unique_id").as_string() == "myio_heating_pca_2010_thr_on");
    CHECK(on.at("state_topic").as_string() == "myio/heating/threshold/pca/2010/on");
    CHECK(on.at("device_class").as_string() == "temperature");
    CHECK(on.at("unit_of_measurement").as_string() == "°C");
    CHECK(on.at("entity_category").as_string() == "diagnostic");
    CHECK(on.at("suggested_display_precision").as_int64() == 1);
    CHECK(!on.contains("command_topic") && !on.contains("state_class"));
    CHECK(on.contains("device") && on.contains("availability"));
    const auto off = json::parse(bridge::make_threshold_discovery(
        ctx, OutputKind::Relay, 3, "", ThresholdKind::Off)).as_object();
    CHECK(off.at("name").as_string() == "myio_heating_relay_3 kikapcsolási küszöb");
    CHECK(off.at("unique_id").as_string() == "myio_heating_relay_3_thr_off");
}

void test_threshold_write_config() {
    CHECK(!config::parse_bridge_config(make_config()).servers[0].threshold_write);
    CHECK(config::parse_bridge_config(make_config("", R"(, "threshold_write": true)")).servers[0].threshold_write);
    CHECK(!config::parse_bridge_config(make_config("", R"(, "threshold_write": false)")).servers[0].threshold_write);
    CHECK(rejected_with(make_config("", R"(, "threshold_write": "yes")"), "threshold_write"));
    CHECK(rejected_with(make_config("", R"(, "threshold_write": 1)"), "threshold_write"));
}

void test_threshold_write() {
    using bridge::OutputKind;
    using bridge::ThresholdKind;
    using bridge::ThresholdTarget;
    namespace json = boost::json;

    // Topics
    CHECK(bridge::threshold_command_topic("myio", "heating", OutputKind::Pca, 2010, ThresholdKind::On)
          == "myio/heating/threshold/pca/2010/on/set");
    CHECK(bridge::threshold_number_discovery_topic("heating", OutputKind::Relay, 3, ThresholdKind::Off)
          == "homeassistant/number/myio_heating_relay_3_thr_off_set/config");

    const auto t1 = bridge::parse_threshold_command_topic("myio/heating/threshold/relay/3/on/set");
    CHECK(t1 && t1->kind == OutputKind::Relay && t1->id == 3 && t1->which == ThresholdKind::On);
    const auto t2 = bridge::parse_threshold_command_topic("myio/heating/threshold/pca/2010/off/set");
    CHECK(t2 && t2->kind == OutputKind::Pca && t2->id == 2010 && t2->which == ThresholdKind::Off);
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/relay/3/on"));        // state topic
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/relay/3/on/set/x"));
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/pwm/3/on/set"));
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/relay/x/on/set"));
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/relay/0/on/set"));
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/threshold/relay/3/mid/set"));
    CHECK(!bridge::parse_threshold_command_topic("myio/heating/switch/relay/3/on/set"));

    // Payload → tenths
    CHECK(bridge::parse_threshold_payload("20.3") == 203);
    CHECK(bridge::parse_threshold_payload("20,3") == 203);
    CHECK(bridge::parse_threshold_payload(" 21 ") == 210);
    CHECK(bridge::parse_threshold_payload("19.75") == 198);
    CHECK(bridge::parse_threshold_payload("0") == 0);
    CHECK(bridge::parse_threshold_payload("90") == 900);
    CHECK(!bridge::parse_threshold_payload(""));
    CHECK(!bridge::parse_threshold_payload("abc"));
    CHECK(!bridge::parse_threshold_payload("20.3x"));
    CHECK(!bridge::parse_threshold_payload("nan"));
    CHECK(!bridge::parse_threshold_payload("-1"));
    CHECK(!bridge::parse_threshold_payload("91"));
    CHECK(!bridge::parse_threshold_payload("90.5"));

    // Device commands
    CHECK(bridge::make_threshold_command({OutputKind::Relay, 3, ThresholdKind::On}, 203)
          == "min_temp_ON*3=203");
    CHECK(bridge::make_threshold_command({OutputKind::Relay, 3, ThresholdKind::Off}, 198)
          == "max_temp_OFF*3=198");
    CHECK(bridge::make_threshold_command({OutputKind::Pca, 2010, ThresholdKind::On}, 210)
          == "PCA_temp_MIN*2010=210");
    CHECK(bridge::make_threshold_command({OutputKind::Pca, 2010, ThresholdKind::Off}, 225)
          == "PCA_temp_MAX*2010=225");

    // Number discovery
    bridge::DiscoveryContext ctx;
    ctx.prefix = "myio"; ctx.server_id = "heating"; ctx.host = "10.0.0.5"; ctx.bridge_version = "v1";
    const auto num = json::parse(bridge::make_threshold_number_discovery(
        ctx, OutputKind::Pca, 2010, "Nappali", ThresholdKind::On)).as_object();
    const auto sensor = json::parse(bridge::make_threshold_discovery(
        ctx, OutputKind::Pca, 2010, "Nappali", ThresholdKind::On)).as_object();
    CHECK(num.at("unique_id").as_string() == "myio_heating_pca_2010_thr_on_set");
    CHECK(num.at("name").as_string() == "Nappali bekapcsolási küszöb (állítás)");
    CHECK(num.at("state_topic") == sensor.at("state_topic"));
    CHECK(num.at("command_topic").as_string() == "myio/heating/threshold/pca/2010/on/set");
    CHECK(num.at("min").as_int64() == 0);
    CHECK(num.at("max").as_int64() == 90);
    CHECK(num.at("step").as_double() == 0.1);
    CHECK(num.at("mode").as_string() == "box");
    CHECK(num.at("unit_of_measurement").as_string() == "°C");
    CHECK(num.at("device_class").as_string() == "temperature");
    CHECK(num.at("entity_category").as_string() == "config");
    CHECK(num.at("device") == sensor.at("device"));
    CHECK(num.at("availability") == sensor.at("availability"));
    CHECK(num.contains("default_entity_id") && num.at("default_entity_id").as_string().starts_with("number."));

    // Disabled: an empty retained payload (removes a previously created entity)
    CHECK(bridge::threshold_number_payload(ctx, OutputKind::Pca, 2010, "Nappali",
                                           ThresholdKind::On, false).empty());
    CHECK(bridge::threshold_number_payload(ctx, OutputKind::Pca, 2010, "Nappali",
                                           ThresholdKind::On, true)
          == bridge::make_threshold_number_discovery(ctx, OutputKind::Pca, 2010, "Nappali",
                                                     ThresholdKind::On));
}

// The slot lines of the real heating controller page (GET /output, 2026.11.03).
constexpr const char* kSlotPage =
    "<script>\n"
    "var actualSlot=0;\n"
    "var bootUpSlot=0;\n"
    "var slotQuantity=3;\n"
    "var slot_description=new Array(3);\n"
    "slot_description [0]='Fűtés';\n"
    "slot_description [1]='Hűtés';\n"
    "slot_description [2]='minden be';\n"
    "var slotExist=[1,1,1,0,0];\n"
    "</script>\n";

void test_js_scalar_and_strings() {
    using bridge::parse_js_int_scalar;
    using bridge::parse_js_indexed_strings;

    // Scalar: real lines, whitespace tolerance, negative, no terminating ';'
    const std::string_view page = kSlotPage;
    CHECK(parse_js_int_scalar(page, "actualSlot") == 0);
    CHECK(parse_js_int_scalar(page, "slotQuantity") == 3);
    CHECK(parse_js_int_scalar("var  a \t=\n 42 ;", "a") == 42);
    CHECK(parse_js_int_scalar("var a=-7;", "a") == -7);
    CHECK(parse_js_int_scalar("var a=5", "a") == 5);

    // Whole identifier only: a prefixed / longer name does not match
    CHECK(!parse_js_int_scalar("var xbootUpSlot=1;", "bootUpSlot"));
    CHECK(!parse_js_int_scalar("var PCA_bootUpSlot=1;", "bootUpSlot"));
    CHECK(!parse_js_int_scalar("var bootUpSlot2=1;", "bootUpSlot"));
    CHECK(parse_js_int_scalar("var xbootUpSlot=1;var bootUpSlot=2;", "bootUpSlot") == 2);
    // Not a `var` declaration / not an integer / missing
    CHECK(!parse_js_int_scalar("bootUpSlot=1;", "bootUpSlot"));
    CHECK(!parse_js_int_scalar("xvar a=1;", "a"));
    CHECK(!parse_js_int_scalar("var a=[1,2];", "a"));
    CHECK(!parse_js_int_scalar("var a=1.5;", "a"));
    CHECK(!parse_js_int_scalar("var a=12abc;", "a"));
    CHECK(!parse_js_int_scalar("var a=", "a"));
    CHECK(!parse_js_int_scalar("", "a"));
    CHECK(!parse_js_int_scalar(page, ""));
    CHECK(!parse_js_int_scalar(page, "noSuchVar"));

    // Indexed strings: UTF-8, the `new Array` declaration ignored
    const auto names = parse_js_indexed_strings(page, "slot_description");
    CHECK(names.size() == 3);
    CHECK(names.at(0) == "Fűtés" && names.at(1) == "Hűtés" && names.at(2) == "minden be");

    // Escapes, whitespace variants, empty string, later entry wins
    const auto esc = parse_js_indexed_strings(
        "n [0]='it\\'s';\nn[1] = 'a\\\\b';\nn [ 2 ]='x';\nn [3]='';\nn [0]='again';", "n");
    CHECK(esc.size() == 3);
    CHECK(esc.at(0) == "again" && esc.at(1) == "a\\b" && esc.at(3) == "");
    CHECK(parse_js_indexed_strings("n [0]='it\\'s';", "n").at(0) == "it's");

    // Exact identifier; comparison and unterminated literal are not entries
    CHECK(parse_js_indexed_strings("xn [0]='a';\nn2 [0]='b';\nPCA_n [0]='c';", "n").empty());
    CHECK(parse_js_indexed_strings("n [0]=='a';", "n").empty());
    CHECK(parse_js_indexed_strings("n [0]='abc\nn [1]='ok';", "n").size() == 1);
    CHECK(parse_js_indexed_strings("n [x]='a';\nn [0]=5;", "n").empty());
    CHECK(parse_js_indexed_strings("", "n").empty());
    CHECK(parse_js_indexed_strings(page, "").empty());
}

void test_active_slot() {
    using bridge::parse_active_slot;
    namespace json = boost::json;

    // The real page: slot 0 = "Fűtés", 3 slots
    const auto s = parse_active_slot(kSlotPage);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->index == 0 && s->name == "Fűtés");
        CHECK(s->boot_index == 0 && s->count == 3 && s->boot_name == "Fűtés");
    }

    // count = number of 1 entries in slotExist, NOT slotQuantity (live lighting controller:
    // actualSlot=3, slotQuantity=2, name in slot_description[3], boot slot 1)
    const auto live = parse_active_slot(
        "var actualSlot=3;var bootUpSlot=1;var slotQuantity=2;\n"
        "var slot_description=new Array(4);\n"
        "slot_description [0]='A';slot_description [1]='Boot';\n"
        "slot_description [3]='Esti';\n"
        "var slotExist=[1,1,1,1,0,0];");
    CHECK(live && live->index == 3 && live->name == "Esti" && live->count == 4);
    CHECK(live && live->boot_index == 1 && live->boot_name == "Boot");
    // slotQuantity alone gives no count; an empty / absent slotExist
    CHECK(!parse_active_slot("var actualSlot=0;var slotQuantity=3;")->count);
    CHECK(parse_active_slot("var actualSlot=0;var slotExist=[];")->count == 0);
    // boot name fallbacks: empty description, unknown index; no bootUpSlot -> no boot name
    const auto b1 = parse_active_slot("var actualSlot=0;var bootUpSlot=2;slot_description [2]=' ';");
    CHECK(b1 && b1->boot_index == 2 && b1->boot_name == "Slot 3");
    const auto b2 = parse_active_slot("var actualSlot=0;var bootUpSlot=7;");
    CHECK(b2 && b2->boot_name == "Slot 8");
    const auto b3 = parse_active_slot("var actualSlot=0;");
    CHECK(b3 && !b3->boot_index && b3->boot_name.empty());

    // Another active slot, boot slot differs
    const auto s2 = parse_active_slot(std::string(kSlotPage) + "var actualSlot=2;var bootUpSlot=1;");
    CHECK(s2 && s2->index == 0);   // the first declaration decides
    const auto s3 = parse_active_slot(
        "var actualSlot=2;var bootUpSlot=1;slot_description [2]=' Éjszaka ';");
    CHECK(s3 && s3->index == 2 && s3->name == "Éjszaka" && s3->boot_index == 1 && !s3->count);

    // Fallback name: no description array, no entry for the index, or an empty one
    const auto f1 = parse_active_slot("var actualSlot=1;");
    CHECK(f1 && f1->name == "Slot 2" && !f1->boot_index && !f1->count);
    const auto f2 = parse_active_slot("var actualSlot=5;slot_description [0]='A';");
    CHECK(f2 && f2->name == "Slot 6");
    const auto f3 = parse_active_slot("var actualSlot=0;slot_description [0]='  ';");
    CHECK(f3 && f3->name == "Slot 1");

    // Missing / unusable actualSlot
    CHECK(!parse_active_slot("<html>nothing</html>"));
    CHECK(!parse_active_slot("var bootUpSlot=0;slot_description [0]='A';"));
    CHECK(!parse_active_slot("var xactualSlot=0;"));
    CHECK(!parse_active_slot("var actualSlot=-1;"));
    CHECK(!parse_active_slot(""));

    // Topics and entity
    CHECK(bridge::slot_state_topic("myio", "heating") == "myio/heating/slot/state");
    CHECK(bridge::slot_attributes_topic("myio", "heating") == "myio/heating/slot/attributes");
    CHECK(bridge::slot_discovery_topic("heating") == "homeassistant/sensor/myio_heating_active_slot/config");

    const auto attrs = json::parse(bridge::make_slot_attributes(*s)).as_object();
    CHECK(attrs.at("index").as_int64() == 0 && attrs.at("boot_index").as_int64() == 0
          && attrs.at("boot_name").as_string() == "Fűtés"
          && attrs.at("count").as_int64() == 3 && attrs.size() == 4);
    const auto partial = json::parse(bridge::make_slot_attributes(*f1)).as_object();
    CHECK(partial.size() == 1 && partial.at("index").as_int64() == 1);

    bridge::DiscoveryContext ctx;
    ctx.prefix = "myio"; ctx.server_id = "heating"; ctx.host = "10.0.0.5"; ctx.bridge_version = "v1";
    const auto d = json::parse(bridge::make_slot_discovery(ctx)).as_object();
    CHECK(d.at("name").as_string() == "Aktív slot");
    CHECK(d.at("unique_id").as_string() == "myio_heating_active_slot");
    CHECK(d.at("state_topic").as_string() == "myio/heating/slot/state");
    CHECK(d.at("json_attributes_topic").as_string() == "myio/heating/slot/attributes");
    CHECK(d.at("icon").as_string() == "mdi:format-list-numbered");
    CHECK(!d.contains("entity_category") && !d.contains("device_class")
          && !d.contains("command_topic") && !d.contains("state_class"));
    CHECK(d.contains("device") && d.contains("availability"));
}

void test_extract_http_body() {
    using bridge::extract_http_body;
    auto throws = [](const std::string& raw) {
        try { (void)extract_http_body(raw, "h"); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    CHECK(extract_http_body("HTTP/1.1 200 OK\r\nA: b\r\n\r\n<html>x\n\ny</html>", "h")
          == "<html>x\n\ny</html>");
    // Bare-LF headers: the body keeps its own blank lines
    CHECK(extract_http_body("HTTP/1.1 200 OK\nA: b\n\n<html>x\r\n\r\ny</html>", "h")
          == "<html>x\r\n\r\ny</html>");
    CHECK(extract_http_body("HTTP/1.1 200 OK\r\n\r\n", "h").empty());
    CHECK(throws("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic\r\n\r\n<html>no</html>"));
    CHECK(throws("HTTP/1.1 404 Not Found\r\n\r\nnope"));
    CHECK(throws("HTTP/1.1 200 OK\r\nno terminator"));
    CHECK(throws(""));
}

} // namespace

int main() {
    test_valid_config();
    test_poll_interval();
    test_coalesce_window();
    test_log_debug();
    test_threshold_write_config();
    test_required_sections();
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
    test_post_verdict();
    test_post_chunking();
    test_extract_json_body();
    test_unreadable_outputs_skipped();
    test_sensor_activator_classification();
    test_reserved_server_id();
    test_availability();
    test_sensor_class();
    test_discovery_blocks();
    test_diagnostics();
    test_mqtt_status();
    test_discovery_merge();
    test_sd_notify();
    test_parse_js_int_array();
    test_thermostat_mapping();
    test_thermostat_mapping_edges();
    test_threshold_entities();
    test_threshold_write();
    test_js_scalar_and_strings();
    test_active_slot();
    test_extract_http_body();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
