#pragma once

#include <string>
#include <unordered_map>

#include "rtmp_server/core/config.hpp"
#include "rtmp_server/core/result.hpp"

namespace rtmp_server::control {

// Renders settings_schema() joined with the config currently active on disk
// at `config_path` into the admin Settings page's JSON body: one object per
// field with its section/label/description/type/restart_required and either
// "value" (plain fields) or "has_value" (sensitive fields — the secret
// itself is never serialised back to a client).
[[nodiscard]] core::Result<std::string> settings_to_json(const std::string& config_path);

// Validates and applies `updates` (raw key -> new value strings, using the
// same keys core::load_config's flat-mapping loader accepts) on top of the
// config file at `config_path`. Written to a temp file first and only
// promoted over the real file if the merged result still loads and passes
// core::ServerConfig::validate() -- an update that would leave the server
// unable to start next time is rejected instead of corrupting the config
// file in place. On success returns the same JSON shape as settings_to_json,
// reflecting the just-written file. Every field takes effect only on the
// next process restart; this function never touches the running server's
// in-memory configuration.
[[nodiscard]] core::Result<std::string> apply_settings_updates(
    const std::string& config_path, const std::unordered_map<std::string, std::string>& updates);

} // namespace rtmp_server::control
