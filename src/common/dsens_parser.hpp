#pragma once

#include "types.hpp"
#include <string_view>

namespace myio {

// ── d_sens_out.json / sens_out.json parser ────────────────────────────────────
//
// Parses the JSON response from the myIO server's d_sens_out.json endpoint.
//
// JSON structure:
//   {
//     "relays":     { "0": { "id":1, "description":"...", "state":0, ... } },
//     "PCA":        { "0": { "id":2001, "description":"...", "state":0, ... } },
//     "PWM":        { "0": { "id":101,  "description":"...", "state":0, ... } },
//     "group":      { "0": { "id":501,  "description":"...", ... } },
//     "joiner":     { "0": { "element0":502, "element1":2009, ... } },
//     "protection": { "0": { "element0":10,  "element1":11, ... } },
//     "sensors":    { "1": { "id":1, "description":"...", "temp":2712 } }
//   }
//
// Array keys ("0", "1", ...) are 0-based indices.
// Real device ids are stored in the "id" field of each object.
// Throws on parse error.
class DSensOutParser {
public:
    [[nodiscard]]
    static ServerState parse(std::string_view json_body, std::string_view server_id);
};

} // namespace myio
