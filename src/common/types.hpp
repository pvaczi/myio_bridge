#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myio {

// ── PCA output (id: 2001–2128) ────────────────────────────────────────────────
// From the myIO server's d_sens_out.json / sens_out.json "PCA" section.
// Array index (0-based key) + 1 = 1-based ordinal, but commands always
// use the real id (e.g. PCA_ON=2005, PCA*2005=50).
struct PcaOutput {
    int         id;                 // real id: 2001–2128
    int         index;              // 0-based JSON array key
    std::string name;               // description field
    int         state;              // current value 0–255
    int         turn_off;           // turnOFF: value when output is OFF (0–255)
    int         turn_on;            // turnON:  value when output is ON  (0–255)
    bool        is_pwm;             // pwm == 1 → brightness controllable
    bool        sensor_controlled;  // sensor != 0
    int         sensor_id;          // controlling sensor id (0 = none)
    int         sensor_on;          // sensorON threshold (×10 → °C or %)
    int         sensor_off;         // sensorOFF threshold (×10)
};

// ── Relay output (id: 1–100) ──────────────────────────────────────────────────
struct RelayOutput {
    int         id;                 // real id: 1–100
    int         index;              // 0-based JSON array key
    std::string name;               // description
    bool        state;              // false = OFF, true = ON
    bool        sensor_controlled;
    int         sensor_id;
    int         sensor_on;
    int         sensor_off;
};

// ── Legacy PWM output (id: 101–113) ──────────────────────────────────────────
struct PwmOutput {
    int         id;                 // 101–113
    int         index;
    std::string name;
    int         state;              // 0–255
    int         turn_off;
    int         turn_on;
    bool        sensor_controlled;
    int         sensor_id;
    int         sensor_on;
    int         sensor_off;
};

// ── Sensor (temperature, humidity, energy, SDM630) ────────────────────────────
// In the JSON "sensors" section, the id is the JSON key:
//   1–100   = temperature
//   101–108 = humidity
//   200     = pulse counter (energy meter)
//   201–203 = SDM630 power (per phase)
//   204–206 = SDM630 voltage (per phase)
//   207–209 = SDM630 current (per phase)
struct Sensor {
    int         id;
    std::string name;
    std::optional<double> temperature;    // temp/100 → °C
    std::optional<double> humidity;       // hum/100  → %
    std::optional<double> energy;         // imp/100  → kWh
    std::optional<double> power;          // P/100    → kW
    std::optional<double> voltage;        // U/100    → V
    std::optional<double> current;        // I/100    → A
};

// ── Group (id: 501–550) ───────────────────────────────────────────────────────
struct Group {
    int              id;
    std::string      name;
    bool             pull_up;       // pullUP flag
    std::vector<int> element_ids;   // real ids of member outputs
};

// ── Protection pair (mutually exclusive output pair) ──────────────────────────
// The myIO server guarantees both outputs are never ON simultaneously.
// Typically used for shutter motors: element0 = UP, element1 = DOWN.
struct ProtectionPair {
    int         index;              // 0-based ordinal
    int         element0;           // first output real id
    int         element1;           // second output real id
    std::string element0_name;      // only present in d_sens_out.json
    std::string element1_name;
};

// ── Joiner pair ───────────────────────────────────────────────────────────────
// When element0 is switched ON/OFF, element1 follows automatically.
struct JoinerPair {
    int         index;
    int         element0;
    int         element1;
    std::string element0_name;
    std::string element1_name;
};

// ── Server info (from /info.json) ────────────────────────────────────────────
struct ServerInfo {
    std::string serial;
    std::string version;
    std::string cpu;
    int         api         = 0;
    int         relay_count = 0;   // "RQ"
    int         pca_count   = 0;   // "PCAQ"
};

// ── Complete polled state of one myIO server ──────────────────────────────────
struct ServerState {
    std::string                  server_id;
    std::string                  server_name;
    std::vector<PcaOutput>       pca_outputs;
    std::vector<RelayOutput>     relay_outputs;
    std::vector<PwmOutput>       pwm_outputs;
    std::vector<Sensor>          sensors;
    std::vector<Group>           groups;
    std::vector<ProtectionPair>  protection_pairs;
    std::vector<JoinerPair>      joiner_pairs;
};

} // namespace myio
