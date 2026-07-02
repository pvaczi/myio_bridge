#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace config {

// ── Output types ──────────────────────────────────────────────────────────────
// Corresponds to the "type" field values in bridge.json.
enum class OutputType {
    Switch,        // "switch"        – simple on/off
    Light,         // "light"         – brightness-controllable (PWM)
    Number,        // "number"        – generic 0–100 value (valve, pump, etc.)
    SensorOutput,  // "sensor_output" – controlled automatically by a sensor
    CoverUp,       // "cover_up"      – UP direction of a cover/shutter
    CoverDown,     // "cover_down"    – DOWN direction of a cover/shutter
};

// ── PCA output configuration ──────────────────────────────────────────────────
struct PcaConfig {
    int         id;
    std::string name;
    OutputType  type;
    bool        enabled;
};

// ── Relay output configuration ────────────────────────────────────────────────
struct RelayConfig {
    int         id;
    std::string name;
    OutputType  type;
    bool        enabled;
};

// ── Legacy PWM output configuration ──────────────────────────────────────────
struct PwmConfig {
    int         id;
    std::string name;
    OutputType  type;
    bool        enabled;
};

// ── Sensor configuration ──────────────────────────────────────────────────────
struct SensorConfig {
    int         id;
    std::string name;
    bool        enabled;
    double      log_threshold = -1.0;  // -1 = use server-level sensor_log_threshold
};

// ── One end of a cover (shutter) ─────────────────────────────────────────────
struct CoverEndpoint {
    std::string output_type;  // "pca" or "relay"
    int         id;
};

// ── Cover (shutter / blind) configuration ────────────────────────────────────
struct CoverConfig {
    std::string   cover_id;
    std::string   name;
    bool          enabled;
    CoverEndpoint up;
    CoverEndpoint down;
    int           motor_timeout_sec = 90;  // myIO PCA timer value; 0 = disable end-position detection
};

// ── Configuration for one myIO server ────────────────────────────────────────
struct ServerConfig {
    std::string           server_id;
    std::string           host;
    std::uint16_t         port             = 80;
    std::string           user             = "admin";
    std::string           password         = "admin";
    int                   poll_interval_sec = 2;
    double                sensor_log_threshold = 0.1;  // min. change to log; 0 = log every publish
    bool                  log_startup_sync     = false; // log entity states during startup/resync
    int                   coalesce_window_ms   = 5;     // wait after 1st command to batch siblings
                                                        // into one POST; 0 = no batching

    // ALL entries are loaded (enabled and disabled alike) so the bridge
    // can send MQTT undiscovery messages for disabled entries.
    std::vector<PcaConfig>    pca;
    std::vector<RelayConfig>  relays;
    std::vector<PwmConfig>    pwm;
    std::vector<SensorConfig> sensors;
    std::vector<CoverConfig>  covers;
};

// ── MQTT broker configuration ─────────────────────────────────────────────────
struct MqttConfig {
    std::string   host         = "localhost";
    std::uint16_t port         = 1883;
    std::string   client_id    = "myio_bridge";
    std::string   user;
    std::string   password;
    std::string   topic_prefix = "myio";
};

// ── Full bridge configuration ─────────────────────────────────────────────────
struct BridgeConfig {
    MqttConfig                mqtt;
    std::vector<ServerConfig> servers;
};

} // namespace config
