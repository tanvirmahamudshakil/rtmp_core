#pragma once

#include <string>
#include <vector>

namespace rtmp_server::control {

// Describes one ServerConfig field for the admin Settings page: the exact
// key core::load_config's flat-mapping parser expects (see config.cpp's
// str/u32/u64/boolean/duration lambdas — this list is hand-kept in sync with
// that function, not derived from it, since C++ has no reflection to do it
// automatically), plus everything the UI needs to render and validate an
// input for it without knowing anything about ServerConfig itself.
struct SettingField {
    enum class Type { String, Bool, U16, U32, U64, DurationMs, Percent };

    std::string key;         // matches config.cpp's flat-mapping key exactly
    std::string section;     // UI grouping, e.g. "Network", "CPU & performance"
    std::string label;       // short human name
    std::string description; // what it does and why you would change it
    Type type = Type::String;
    // Never echoed back in a GET response body (only a has_value flag is),
    // and the admin UI renders it as a password-style input that starts
    // empty rather than pre-filled with the current secret.
    bool sensitive = false;
    // Every field here is only read at process startup (core::load_config is
    // called once in main()), so this is true for all of them today. Kept as
    // a per-field flag rather than a blanket UI note in case a future field
    // is wired to take effect live.
    bool restart_required = true;
};

// Hand-maintained, one entry per core::ServerConfig field that
// core::load_config's flat-mapping loader recognises. Order is presentation
// order (grouped by section) for the admin UI, not declaration order in
// config.hpp.
const std::vector<SettingField>& settings_schema();

} // namespace rtmp_server::control
