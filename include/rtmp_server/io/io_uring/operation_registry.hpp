#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/random.hpp"
#include "rtmp_server/io/io_uring/operation.hpp"

namespace rtmp_server::io::io_uring {

// Owns OperationContext instances so they remain heap-stable for the
// lifetime of an in-flight SQE/CQE pair. The io_uring user_data field holds
// only the operation_id (a plain uint64_t) — never a raw pointer — and this
// registry is the sole place that resolves an id back to its context.
class OperationRegistry {
public:
    [[nodiscard]] std::uint64_t create(OperationContext context) {
        auto id = core::generate_id64();
        std::lock_guard<std::mutex> lock(mutex_);
        context.operation_id = id;
        operations_.emplace(id, std::move(context));
        return id;
    }

    [[nodiscard]] std::optional<OperationContext> find(std::uint64_t operation_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = operations_.find(operation_id);
        if (it == operations_.end()) return std::nullopt;
        return it->second;
    }

    // Removes and returns the context. Called exactly once per operation,
    // when its terminal completion (success, error, or cancellation) is
    // processed — never before, since the SQE may still be in flight.
    [[nodiscard]] std::optional<OperationContext> take(std::uint64_t operation_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = operations_.find(operation_id);
        if (it == operations_.end()) return std::nullopt;
        auto context = std::move(it->second);
        operations_.erase(it);
        return context;
    }

    [[nodiscard]] std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return operations_.size();
    }

    // All operation IDs currently in flight for a connection — used to
    // issue IORING_OP_ASYNC_CANCEL against each on close (docs/rtmp_promot.md
    // "Connection shutdown sequence"). Read-only: does not remove entries,
    // since the SQE cancel request is what triggers their eventual
    // completion and removal via take().
    [[nodiscard]] std::vector<std::uint64_t> find_all_for_connection(std::uint64_t connection_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint64_t> ids;
        for (const auto& [id, ctx] : operations_) {
            if (ctx.connection_id == connection_id) ids.push_back(id);
        }
        return ids;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, OperationContext> operations_;
};

} // namespace rtmp_server::io::io_uring
