#pragma once

#include <memory>
#include <string>

#include "rtmp_server/persistence/store.hpp"

// Forward-declared rather than #include <sqlite3.h> here: keeps the SQLite
// C API out of every translation unit that only needs the Store interface
// (mirrors how io_uring headers stay out of the protocol layer).
struct sqlite3;

namespace rtmp_server::persistence {

// SQLite-backed Store — the development/single-node persistence engine
// (docs/rtmp_promot.md Phase 9 "SQLite development persistence"). Opens (and
// creates, with `CREATE TABLE IF NOT EXISTS`) a two-table schema
// (applications, streams) at construction. Not safe for concurrent use from
// multiple threads without external synchronization (same posture as every
// other component here — the management layer that owns this already
// serializes access via StreamManager's own mutex).
class SqliteStore : public Store {
public:
    // `path` may be a filesystem path or ":memory:" for an ephemeral
    // in-process database (used by tests). Returns an error instead of
    // throwing if the file can't be opened or the schema can't be created —
    // matches every other fallible-at-construction component's Result-
    // returning factory convention in this codebase (e.g. core::load_config).
    [[nodiscard]] static core::Result<std::unique_ptr<SqliteStore>> open(const std::string& path);

    ~SqliteStore() override;
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    core::Result<void> upsert_application(const ApplicationRow& row) override;
    core::Result<void> delete_application(std::string_view name) override;
    [[nodiscard]] core::Result<std::vector<ApplicationRow>> load_applications() override;

    core::Result<void> upsert_stream(const StreamRow& row) override;
    core::Result<void> delete_stream(std::string_view application, std::string_view name) override;
    [[nodiscard]] core::Result<std::vector<StreamRow>> load_streams() override;

private:
    explicit SqliteStore(sqlite3* db) noexcept : db_(db) {}

    sqlite3* db_;
};

} // namespace rtmp_server::persistence
