#include "rtmp_server/observability/audit_log.hpp"

namespace rtmp_server::observability {

void AuditLog::record(AuditEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() >= max_entries_) entries_.pop_front(); // drop-oldest
    entries_.push_back(std::move(entry));
}

std::vector<AuditEntry> AuditLog::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<AuditEntry>(entries_.begin(), entries_.end());
}

std::size_t AuditLog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace rtmp_server::observability
