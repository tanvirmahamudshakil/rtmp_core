#pragma once

#include <memory>
#include <mutex>
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
// (applications, streams) at construction. Calls are serialized internally:
// the management HTTP pool may run a readiness read concurrently with a
// StreamManager write, and sharing one SQLite connection without this guard
// would make correctness depend on how the distro compiled libsqlite.
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
    core::Result<void> upsert_stream_target(const StreamTargetRow& row) override;
    core::Result<void> delete_stream_target(std::string_view application, std::string_view stream,
                                            std::string_view name) override;
    [[nodiscard]] core::Result<std::vector<StreamTargetRow>> load_stream_targets() override;

    core::Result<void> upsert_backup_publisher(const BackupPublisherRow& row) override;
    core::Result<void> delete_backup_publisher(std::string_view application,
                                               std::string_view stream) override;
    [[nodiscard]] core::Result<std::vector<BackupPublisherRow>> load_backup_publishers() override;

    core::Result<void> upsert_transcoder_job(const TranscoderJobRow& row) override;
    core::Result<void> delete_transcoder_job(std::string_view application,
                                             std::string_view name) override;
    [[nodiscard]] core::Result<std::vector<TranscoderJobRow>> load_transcoder_jobs() override;

    core::Result<void> upsert_cluster_node(const ClusterNodeRow& row) override;
    core::Result<void> delete_cluster_node(std::string_view id) override;
    core::Result<void> update_cluster_node_forced_draining(std::string_view id, bool draining) override;
    [[nodiscard]] core::Result<std::vector<ClusterNodeRow>> load_cluster_nodes() override;

    core::Result<void> upsert_transcoding_assignment(const TranscodingAssignmentRow& row) override;
    core::Result<void> delete_transcoding_assignment(std::string_view application,
                                                      std::string_view source_stream) override;
    [[nodiscard]] core::Result<std::vector<TranscodingAssignmentRow>>
    load_transcoding_assignments() override;

    core::Result<void> upsert_template(const TemplateRow& row) override;
    core::Result<void> delete_template(std::string_view id) override;
    [[nodiscard]] core::Result<std::vector<TemplateRow>> load_templates() override;

    core::Result<void> upsert_source_job(const SourceJobRow& row) override;
    core::Result<void> delete_source_job(std::string_view application, std::string_view name) override;
    [[nodiscard]] core::Result<std::vector<SourceJobRow>> load_source_jobs() override;

private:
    explicit SqliteStore(sqlite3* db) noexcept : db_(db) {}

    sqlite3* db_;
    std::mutex mutex_;
};

} // namespace rtmp_server::persistence
