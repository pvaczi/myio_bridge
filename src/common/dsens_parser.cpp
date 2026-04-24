#include <dsens_parser.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>

namespace myio {

namespace json = boost::json;

// ── Helper functions ──────────────────────────────────────────────────────────

static std::string str_or(const json::object& o, std::string_view key,
                           std::string_view fallback = "") {
    if (const auto* v = o.if_contains(key))
        if (const auto* s = v->if_string())
            return { s->begin(), s->end() };
    return std::string(fallback);
}

static int int_or(const json::object& o, std::string_view key, int fallback = 0) {
    if (const auto* v = o.if_contains(key)) {
        if (const auto* n = v->if_int64())  return static_cast<int>(*n);
        if (const auto* n = v->if_uint64()) return static_cast<int>(*n);
    }
    return fallback;
}

static bool bool_flag(const json::object& o, std::string_view key) {
    return int_or(o, key) != 0;
}

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, not_space));
    s.erase(std::ranges::find_if(s | std::views::reverse, not_space).base(), s.end());
    return s;
}

// ── Section parsers ───────────────────────────────────────────────────────────

static void parse_relays(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        RelayOutput r;
        r.index             = std::stoi(std::string(key));
        r.id                = int_or(*entry, "id", r.index + 1);
        r.name              = trim(str_or(*entry, "description"));
        r.state             = int_or(*entry, "state") != 0;
        r.sensor_id         = int_or(*entry, "sensor");
        r.sensor_controlled = r.sensor_id != 0;
        r.sensor_on         = int_or(*entry, "sensorON");
        r.sensor_off        = int_or(*entry, "sensorOFF");
        state.relay_outputs.push_back(std::move(r));
    }
    std::ranges::sort(state.relay_outputs, {}, &RelayOutput::index);
}

static void parse_pca(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        PcaOutput p;
        p.index             = std::stoi(std::string(key));
        p.id                = int_or(*entry, "id", 2001 + p.index);
        p.name              = trim(str_or(*entry, "description"));
        p.state             = int_or(*entry, "state");
        p.turn_off          = int_or(*entry, "turnOFF");
        p.turn_on           = int_or(*entry, "turnON", 255);
        p.is_pwm            = bool_flag(*entry, "pwm");
        p.timer_sec         = int_or(*entry, "timer");
        p.sensor_id         = int_or(*entry, "sensor");
        p.sensor_controlled = p.sensor_id != 0;
        p.sensor_on         = int_or(*entry, "sensorON");
        p.sensor_off        = int_or(*entry, "sensorOFF");
        state.pca_outputs.push_back(std::move(p));
    }
    std::ranges::sort(state.pca_outputs, {}, &PcaOutput::index);
}

static void parse_pwm(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        PwmOutput p;
        p.index             = std::stoi(std::string(key));
        p.id                = int_or(*entry, "id", 101 + p.index);
        p.name              = trim(str_or(*entry, "description"));
        p.state             = int_or(*entry, "state");
        p.turn_off          = int_or(*entry, "turnOFF");
        p.turn_on           = int_or(*entry, "turnON", 255);
        p.sensor_id         = int_or(*entry, "sensor");
        p.sensor_controlled = p.sensor_id != 0;
        p.sensor_on         = int_or(*entry, "sensorON");
        p.sensor_off        = int_or(*entry, "sensorOFF");
        state.pwm_outputs.push_back(std::move(p));
    }
    std::ranges::sort(state.pwm_outputs, {}, &PwmOutput::index);
}

static void parse_sensors(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        Sensor s;
        s.id   = int_or(*entry, "id", std::stoi(std::string(key)));
        s.name = trim(str_or(*entry, "description"));

        if (s.id >= 1 && s.id <= 100) {
            if (const auto* v = entry->if_contains("temp"))
                if (const auto* n = v->if_int64())
                    s.temperature = *n / 100.0;
        } else if (s.id >= 101 && s.id <= 108) {
            if (const auto* v = entry->if_contains("hum"))
                if (const auto* n = v->if_int64())
                    s.humidity = *n / 100.0;
        } else if (s.id == 200) {
            if (const auto* v = entry->if_contains("imp"))
                if (const auto* n = v->if_int64())
                    s.energy = *n / 100.0;
        } else if (s.id >= 201 && s.id <= 203) {
            if (const auto* v = entry->if_contains("P"))
                if (const auto* n = v->if_int64())
                    s.power = *n / 100.0;
        } else if (s.id >= 204 && s.id <= 206) {
            if (const auto* v = entry->if_contains("U"))
                if (const auto* n = v->if_int64())
                    s.voltage = *n / 100.0;
        } else if (s.id >= 207 && s.id <= 209) {
            if (const auto* v = entry->if_contains("I"))
                if (const auto* n = v->if_int64())
                    s.current = *n / 100.0;
        }

        state.sensors.push_back(std::move(s));
    }
    std::ranges::sort(state.sensors, {}, &Sensor::id);
}

static void parse_groups(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        Group g;
        g.id      = int_or(*entry, "id");
        g.name    = trim(str_or(*entry, "description"));
        g.pull_up = bool_flag(*entry, "pullUP");

        for (int i = 0; i < 10; ++i) {
            const std::string elem_key = "element" + std::to_string(i);
            if (!entry->contains(elem_key)) break;
            const int elem_id = int_or(*entry, elem_key);
            if (elem_id == 0) break;
            g.element_ids.push_back(elem_id);
        }

        state.groups.push_back(std::move(g));
    }
    std::ranges::sort(state.groups, {}, &Group::id);
}

static void parse_protection(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        ProtectionPair p;
        p.index         = std::stoi(std::string(key));
        p.element0      = int_or(*entry, "element0");
        p.element1      = int_or(*entry, "element1");
        p.element0_name = trim(str_or(*entry, "element0d"));
        p.element1_name = trim(str_or(*entry, "element1d"));

        if (p.element0 != 0 && p.element1 != 0)
            state.protection_pairs.push_back(std::move(p));
    }
    std::ranges::sort(state.protection_pairs, {}, &ProtectionPair::index);
}

static void parse_joiners(const json::object& obj, ServerState& state) {
    for (const auto& [key, val] : obj) {
        const auto* entry = val.if_object();
        if (!entry) continue;

        JoinerPair j;
        j.index         = std::stoi(std::string(key));
        j.element0      = int_or(*entry, "element0");
        j.element1      = int_or(*entry, "element1");
        j.element0_name = trim(str_or(*entry, "element0d"));
        j.element1_name = trim(str_or(*entry, "element1d"));

        if (j.element0 != 0 && j.element1 != 0)
            state.joiner_pairs.push_back(std::move(j));
    }
    std::ranges::sort(state.joiner_pairs, {}, &JoinerPair::index);
}

// ── Main parse function ───────────────────────────────────────────────────────

ServerState DSensOutParser::parse(std::string_view json_body,
                                   std::string_view server_id) {
    json::value root;
    try {
        root = json::parse(json_body);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("d_sens_out.json parse error: ") + e.what());
    }

    const auto* obj = root.if_object();
    if (!obj)
        throw std::runtime_error("d_sens_out.json: root is not an object");

    ServerState state;
    state.server_id = std::string(server_id);

    if (const auto* v = obj->if_contains("relays"))
        if (const auto* o = v->if_object()) parse_relays(*o, state);

    if (const auto* v = obj->if_contains("PCA"))
        if (const auto* o = v->if_object()) parse_pca(*o, state);

    if (const auto* v = obj->if_contains("PWM"))
        if (const auto* o = v->if_object()) parse_pwm(*o, state);

    if (const auto* v = obj->if_contains("sensors"))
        if (const auto* o = v->if_object()) parse_sensors(*o, state);

    if (const auto* v = obj->if_contains("group"))
        if (const auto* o = v->if_object()) parse_groups(*o, state);

    if (const auto* v = obj->if_contains("protection"))
        if (const auto* o = v->if_object()) parse_protection(*o, state);

    if (const auto* v = obj->if_contains("joiner"))
        if (const auto* o = v->if_object()) parse_joiners(*o, state);

    return state;
}

} // namespace myio
