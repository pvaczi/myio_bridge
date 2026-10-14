#pragma once
// Pure bridge.json merge logic for the discovery tool (no network, no file I/O).
// Existing user-edited config wins; device-derived facts are refreshed.

#include <boost/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace discovery {

namespace json = boost::json;

// Human-readable lines describing what the merge noticed (new / vanished outputs, kept configs).
using MergeReport = std::vector<std::string>;

// Merges a freshly discovered server entry into the existing one (nullptr = none).
//   - server-level keys: existing value wins; keys missing from the existing entry are added;
//     a stale `_error` (stub of an earlier failed run) is dropped, as the server answered now
//   - entries in pca/relays/pwm/sensors/covers are matched by id; the existing entry is the base
//     (every user-set key and unknown key survives); `name` and "_"-prefixed diagnostics are
//     refreshed from the device; keys missing from the existing entry are added
//   - new device entries are appended with their discovery defaults
//   - existing entries the device no longer reports are KEPT and reported
json::object merge_server(const json::object* existing, const json::object& fresh,
                          MergeReport& report);

// Result for a server that could not be queried: an existing config is kept UNCHANGED (and the
// failure reported); only a server without any existing config gets the `_error` stub.
json::object failed_server(const json::object* existing, const std::string& id,
                           const std::string& host, const std::string& error,
                           MergeReport& report);

// Finds the server object with the given id in a parsed bridge.json root (nullptr if none).
const json::object* find_server(const json::object& root, std::string_view id);

} // namespace discovery
