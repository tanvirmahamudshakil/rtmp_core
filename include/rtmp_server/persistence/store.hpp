#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::persistence {

// Plain data rows persisted for the management domain (Phase 8's
// management::Application/Stream), deliberately not the same types as
// management::Application/Stream: persistence should not force the domain
// layer to shape its structs around a storage engine, and vice versa —
// StreamManager maps between the two at its persistence call sites.
struct ApplicationRow {
    std::string name;
    bool enabled = true;
};

struct StreamRow {
    std::string application;
    std::string name;
    std::string key_hash; // sha256_hex of the raw key — see core/hmac.hpp
    bool enabled = true;
    bool recording_enabled = false;
    std::int64_t created_at_unix = 0;
};

struct TranscodingAssignmentRow {
    std::string application;
    std::string source_stream;
    std::string template_name;
    std::string rules;
};

// Storage-engine-independent persistence contract for the management
// domain (Phase 9, docs/rtmp_promot.md "Persistence"). Deliberately narrow —
// just enough for StreamManager to survive a restart — not a general ORM.
// Implementations must never block the RTMP media path: every call here is
// expected to be invoked only from management-API-rate code (create/rotate/
// enable/disable/delete), never per-packet (see docs/rtmp_promot.md
// "Persistence" — "Do not perform blocking database queries for every media
// packet").
class Store {
public:
    virtual ~Store() = default;

    virtual core::Result<void> upsert_application(const ApplicationRow& row) = 0;
    virtual core::Result<void> delete_application(std::string_view name) = 0;
    [[nodiscard]] virtual core::Result<std::vector<ApplicationRow>> load_applications() = 0;

    virtual core::Result<void> upsert_stream(const StreamRow& row) = 0;
    virtual core::Result<void> delete_stream(std::string_view application, std::string_view name) = 0;
    [[nodiscard]] virtual core::Result<std::vector<StreamRow>> load_streams() = 0;

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_transcoding_assignment(const TranscodingAssignmentRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_transcoding_assignment(std::string_view, std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<TranscodingAssignmentRow>>
    load_transcoding_assignments() {
        return std::vector<TranscodingAssignmentRow>{};
    }
};

} // namespace rtmp_server::persistence
