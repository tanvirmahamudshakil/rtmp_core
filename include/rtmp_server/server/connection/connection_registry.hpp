#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "rtmp_server/network/tcp_connection.hpp"

namespace rtmp_server::server {

// The canonical STRONG owner of every live TcpConnection. OperationContext
// (io_uring/operation.hpp) intentionally holds only a weak_ptr, so a
// connection's lifetime is governed entirely by this registry: add() on
// accept, remove() on close. Without a strong owner somewhere, a
// TcpConnection would be destroyed as soon as the accept handler's local
// shared_ptr went out of scope, while operations referencing it were still
// in flight — this registry is what prevents that.
class ConnectionRegistry {
public:
    void add(std::shared_ptr<network::TcpConnection> connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[connection->connection_id()] = std::move(connection);
    }

    void remove(std::uint64_t connection_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(connection_id);
    }

    [[nodiscard]] std::shared_ptr<network::TcpConnection> find(std::uint64_t connection_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(connection_id);
        if (it == connections_.end()) return nullptr;
        return it->second;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

    // Snapshot of all live connections, used for graceful shutdown fan-out.
    [[nodiscard]] std::vector<std::shared_ptr<network::TcpConnection>> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<network::TcpConnection>> out;
        out.reserve(connections_.size());
        for (auto& [id, conn] : connections_) out.push_back(conn);
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<network::TcpConnection>> connections_;
};

} // namespace rtmp_server::server
