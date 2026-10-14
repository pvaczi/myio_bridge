#include "bridge_rules.hpp"

#include <charconv>
#include <string>

namespace bridge {

namespace {

constexpr bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back()))  s.remove_suffix(1);
    return s;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

} // namespace

std::string_view command_component(OutputKind kind, config::OutputType type) {
    using config::OutputType;
    switch (kind) {
        case OutputKind::Pca:
            switch (type) {
                case OutputType::Light:     return "light";
                case OutputType::Number:    return "number";
                case OutputType::CoverUp:
                case OutputType::CoverDown: return "";
                default:                    return "switch";  // Switch, SensorOutput
            }
        case OutputKind::Relay:
            // Relays are plain on/off outputs: every non-cover type is a switch.
            if (type == OutputType::CoverUp || type == OutputType::CoverDown) return "";
            return "switch";
        case OutputKind::Pwm:
            // Legacy PWM: number or (dimmable) light — matches the discovery published so far.
            return type == OutputType::Number ? "number" : "light";
    }
    return "";
}

std::string_view ha_component(OutputKind kind, config::OutputType type, bool writable) {
    const auto cmd = command_component(kind, type);
    if (writable || cmd.empty()) return cmd;
    if (cmd == "switch") return "binary_sensor";
    return "sensor";  // light, number → percentage sensor
}

std::optional<bool> parse_on_off(std::string_view payload) {
    const auto p = upper(trim(payload));
    if (p == "ON")  return true;
    if (p == "OFF") return false;
    return std::nullopt;
}

std::optional<int> parse_int_in_range(std::string_view payload, int lo, int hi) {
    const auto p = trim(payload);
    if (p.empty()) return std::nullopt;
    int value = 0;
    const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
    if (ec != std::errc{} || ptr != p.data() + p.size()) return std::nullopt;
    if (value < lo || value > hi) return std::nullopt;
    return value;
}

std::optional<CoverCommand> parse_cover_command(std::string_view payload) {
    const auto p = upper(trim(payload));
    if (p == "OPEN")  return CoverCommand::Open;
    if (p == "CLOSE") return CoverCommand::Close;
    if (p == "STOP")  return CoverCommand::Stop;
    return std::nullopt;
}

std::optional<int> parse_http_status(std::string_view raw) {
    // Status line only: a search over the whole response would misfire on
    // header values such as "Content-Length: 1401".
    const auto eol = raw.find('\n');
    std::string_view line = raw.substr(0, eol);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (!line.starts_with("HTTP/")) return std::nullopt;
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    line.remove_prefix(sp);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);

    // Exactly three digits, followed by end of line or a separator.
    if (line.size() < 3) return std::nullopt;
    for (std::size_t i = 0; i < 3; ++i)
        if (line[i] < '0' || line[i] > '9') return std::nullopt;
    if (line.size() > 3 && line[3] != ' ' && line[3] != '\t') return std::nullopt;
    const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (code < 100) return std::nullopt;
    return code;
}

} // namespace bridge
