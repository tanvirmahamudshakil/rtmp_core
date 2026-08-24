#include "rtmp_server/persistence/sqlite_store.hpp"

#include <sqlite3.h>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::persistence {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

namespace {

constexpr const char* kSchema = R"sql(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=5000;
CREATE TABLE IF NOT EXISTS applications (
    name TEXT PRIMARY KEY,
    enabled INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS streams (
    application TEXT NOT NULL,
    name TEXT NOT NULL,
    key_hash TEXT NOT NULL,
    enabled INTEGER NOT NULL,
    recording_enabled INTEGER NOT NULL,
    created_at_unix INTEGER NOT NULL,
    PRIMARY KEY (application, name)
);
CREATE TABLE IF NOT EXISTS transcoding_assignments (
    application TEXT NOT NULL,
    source_stream TEXT NOT NULL,
    template_name TEXT NOT NULL,
    rules TEXT NOT NULL,
    PRIMARY KEY (application, source_stream)
);
CREATE TABLE IF NOT EXISTS templates (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    presets TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS source_jobs (
    application TEXT NOT NULL,
    name TEXT NOT NULL,
    source_url TEXT NOT NULL,
    template_name TEXT NOT NULL,
    rules TEXT NOT NULL,
    auto_restart INTEGER NOT NULL,
    restart_delay_seconds INTEGER NOT NULL,
    enabled INTEGER NOT NULL,
    PRIMARY KEY (application, name)
);
)sql";

Error sqlite_error(sqlite3* db, std::string_view context) {
    std::string message(context);
    message += ": ";
    message += db != nullptr ? sqlite3_errmsg(db) : "unknown sqlite error";
    return Error(ErrorCode::StorageWriteFailed, ErrorCategory::Storage, message);
}

// RAII wrapper so every early-return path below still finalizes the
// statement — sqlite3 has no exceptions to unwind through.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool valid() const noexcept { return stmt_ != nullptr; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace

Result<std::unique_ptr<SqliteStore>> SqliteStore::open(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        Error err = sqlite_error(db, "sqlite3_open");
        sqlite3_close(db);
        return err;
    }

    char* errmsg = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        Error err(ErrorCode::StorageWriteFailed, ErrorCategory::Storage,
                   std::string("schema creation failed: ") + (errmsg != nullptr ? errmsg : "unknown"));
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return err;
    }

    return std::unique_ptr<SqliteStore>(new SqliteStore(db));
}

SqliteStore::~SqliteStore() {
    if (db_ != nullptr) sqlite3_close(db_);
}

Result<void> SqliteStore::upsert_application(const ApplicationRow& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT INTO applications (name, enabled) VALUES (?, ?) "
                        "ON CONFLICT(name) DO UPDATE SET enabled = excluded.enabled");
    if (!stmt.valid()) return sqlite_error(db_, "prepare upsert_application");

    sqlite3_bind_text(stmt.get(), 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, row.enabled ? 1 : 0);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step upsert_application");
    return Result<void>{};
}

Result<void> SqliteStore::delete_application(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "DELETE FROM applications WHERE name = ?");
    if (!stmt.valid()) return sqlite_error(db_, "prepare delete_application");

    sqlite3_bind_text(stmt.get(), 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step delete_application");
    return Result<void>{};
}

Result<std::vector<ApplicationRow>> SqliteStore::load_applications() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT name, enabled FROM applications");
    if (!stmt.valid()) return sqlite_error(db_, "prepare load_applications");

    std::vector<ApplicationRow> out;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        ApplicationRow row;
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.enabled = sqlite3_column_int(stmt.get(), 1) != 0;
        out.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) return sqlite_error(db_, "step load_applications");
    return out;
}

Result<void> SqliteStore::upsert_stream(const StreamRow& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT INTO streams (application, name, key_hash, enabled, recording_enabled, "
                        "created_at_unix) VALUES (?, ?, ?, ?, ?, ?) "
                        "ON CONFLICT(application, name) DO UPDATE SET "
                        "key_hash = excluded.key_hash, enabled = excluded.enabled, "
                        "recording_enabled = excluded.recording_enabled");
    if (!stmt.valid()) return sqlite_error(db_, "prepare upsert_stream");

    sqlite3_bind_text(stmt.get(), 1, row.application.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, row.key_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, row.enabled ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 5, row.recording_enabled ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 6, row.created_at_unix);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step upsert_stream");
    return Result<void>{};
}

Result<void> SqliteStore::delete_stream(std::string_view application, std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "DELETE FROM streams WHERE application = ? AND name = ?");
    if (!stmt.valid()) return sqlite_error(db_, "prepare delete_stream");

    sqlite3_bind_text(stmt.get(), 1, application.data(), static_cast<int>(application.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step delete_stream");
    return Result<void>{};
}

Result<std::vector<StreamRow>> SqliteStore::load_streams() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT application, name, key_hash, enabled, recording_enabled, created_at_unix "
                        "FROM streams");
    if (!stmt.valid()) return sqlite_error(db_, "prepare load_streams");

    std::vector<StreamRow> out;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        StreamRow row;
        row.application = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        row.key_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        row.enabled = sqlite3_column_int(stmt.get(), 3) != 0;
        row.recording_enabled = sqlite3_column_int(stmt.get(), 4) != 0;
        row.created_at_unix = sqlite3_column_int64(stmt.get(), 5);
        out.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) return sqlite_error(db_, "step load_streams");
    return out;
}

Result<void> SqliteStore::upsert_transcoding_assignment(const TranscodingAssignmentRow& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT INTO transcoding_assignments "
                        "(application, source_stream, template_name, rules) VALUES (?, ?, ?, ?) "
                        "ON CONFLICT(application, source_stream) DO UPDATE SET "
                        "template_name = excluded.template_name, rules = excluded.rules");
    if (!stmt.valid()) return sqlite_error(db_, "prepare upsert_transcoding_assignment");
    sqlite3_bind_text(stmt.get(), 1, row.application.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, row.source_stream.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, row.template_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, row.rules.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return sqlite_error(db_, "step upsert_transcoding_assignment");
    }
    return {};
}

Result<void> SqliteStore::delete_transcoding_assignment(std::string_view application,
                                                         std::string_view source_stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "DELETE FROM transcoding_assignments "
                        "WHERE application = ? AND source_stream = ?");
    if (!stmt.valid()) return sqlite_error(db_, "prepare delete_transcoding_assignment");
    sqlite3_bind_text(stmt.get(), 1, application.data(), static_cast<int>(application.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, source_stream.data(), static_cast<int>(source_stream.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return sqlite_error(db_, "step delete_transcoding_assignment");
    }
    return {};
}

Result<std::vector<TranscodingAssignmentRow>> SqliteStore::load_transcoding_assignments() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT application, source_stream, template_name, rules "
                        "FROM transcoding_assignments ORDER BY application, source_stream");
    if (!stmt.valid()) return sqlite_error(db_, "prepare load_transcoding_assignments");
    std::vector<TranscodingAssignmentRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        TranscodingAssignmentRow row;
        row.application = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.source_stream = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        row.template_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        row.rules = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) return sqlite_error(db_, "step load_transcoding_assignments");
    return rows;
}

Result<void> SqliteStore::upsert_template(const TemplateRow& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT INTO templates (id, name, presets) VALUES (?, ?, ?) "
                        "ON CONFLICT(id) DO UPDATE SET name = excluded.name, presets = excluded.presets");
    if (!stmt.valid()) return sqlite_error(db_, "prepare upsert_template");
    sqlite3_bind_text(stmt.get(), 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, row.presets_json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step upsert_template");
    return {};
}

Result<void> SqliteStore::delete_template(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "DELETE FROM templates WHERE id = ?");
    if (!stmt.valid()) return sqlite_error(db_, "prepare delete_template");
    sqlite3_bind_text(stmt.get(), 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step delete_template");
    return {};
}

Result<std::vector<TemplateRow>> SqliteStore::load_templates() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT id, name, presets FROM templates ORDER BY name");
    if (!stmt.valid()) return sqlite_error(db_, "prepare load_templates");
    std::vector<TemplateRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        TemplateRow row;
        row.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        row.presets_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) return sqlite_error(db_, "step load_templates");
    return rows;
}

Result<void> SqliteStore::upsert_source_job(const SourceJobRow& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT INTO source_jobs "
                        "(application, name, source_url, template_name, rules, auto_restart, "
                        "restart_delay_seconds, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                        "ON CONFLICT(application, name) DO UPDATE SET "
                        "source_url = excluded.source_url, template_name = excluded.template_name, "
                        "rules = excluded.rules, auto_restart = excluded.auto_restart, "
                        "restart_delay_seconds = excluded.restart_delay_seconds, "
                        "enabled = excluded.enabled");
    if (!stmt.valid()) return sqlite_error(db_, "prepare upsert_source_job");
    sqlite3_bind_text(stmt.get(), 1, row.application.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, row.source_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, row.template_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, row.rules.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 6, row.auto_restart ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 7, static_cast<int>(row.restart_delay_seconds));
    sqlite3_bind_int(stmt.get(), 8, row.enabled ? 1 : 0);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step upsert_source_job");
    return {};
}

Result<void> SqliteStore::delete_source_job(std::string_view application, std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "DELETE FROM source_jobs WHERE application = ? AND name = ?");
    if (!stmt.valid()) return sqlite_error(db_, "prepare delete_source_job");
    sqlite3_bind_text(stmt.get(), 1, application.data(), static_cast<int>(application.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return sqlite_error(db_, "step delete_source_job");
    return {};
}

Result<std::vector<SourceJobRow>> SqliteStore::load_source_jobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT application, name, source_url, template_name, rules, auto_restart, "
                        "restart_delay_seconds, enabled FROM source_jobs ORDER BY application, name");
    if (!stmt.valid()) return sqlite_error(db_, "prepare load_source_jobs");
    std::vector<SourceJobRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        SourceJobRow row;
        row.application = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        row.source_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        row.template_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        row.rules = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        row.auto_restart = sqlite3_column_int(stmt.get(), 5) != 0;
        row.restart_delay_seconds = static_cast<std::uint32_t>(sqlite3_column_int(stmt.get(), 6));
        row.enabled = sqlite3_column_int(stmt.get(), 7) != 0;
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) return sqlite_error(db_, "step load_source_jobs");
    return rows;
}

} // namespace rtmp_server::persistence
