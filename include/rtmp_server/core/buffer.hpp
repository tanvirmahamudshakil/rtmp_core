#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace rtmp_server::core {

// Immutable, reference-counted payload shared between the transport layer,
// protocol layer, GOP cache, and N subscriber queues without copying.
// Immutability is what makes sharing across subscribers safe without locks.
class SharedBuffer {
public:
    SharedBuffer() = default;

    static SharedBuffer copy_from(std::span<const std::byte> data) {
        return SharedBuffer(std::make_shared<const std::vector<std::byte>>(data.begin(), data.end()));
    }

    static SharedBuffer adopt(std::vector<std::byte> data) {
        return SharedBuffer(std::make_shared<const std::vector<std::byte>>(std::move(data)));
    }

    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return storage_ ? std::span<const std::byte>(*storage_) : std::span<const std::byte>{};
    }

    [[nodiscard]] std::size_t size() const noexcept { return storage_ ? storage_->size() : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] long use_count() const noexcept { return storage_.use_count(); }

private:
    explicit SharedBuffer(std::shared_ptr<const std::vector<std::byte>> storage)
        : storage_(std::move(storage)) {}

    std::shared_ptr<const std::vector<std::byte>> storage_;
};

// A mutable, fixed-capacity byte buffer owned by a BufferPool. Bounds-safe:
// write() refuses to exceed capacity rather than reallocating, so pool
// entries never grow past their configured size.
class ByteBuffer {
public:
    explicit ByteBuffer(std::size_t capacity) : data_(capacity) {}

    [[nodiscard]] std::span<std::byte> writable_span() noexcept {
        return std::span<std::byte>(data_.data() + size_, data_.size() - size_);
    }

    [[nodiscard]] std::span<const std::byte> readable_span() const noexcept {
        return std::span<const std::byte>(data_.data(), size_);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Commits `n` bytes previously written into writable_span() as readable.
    // Returns false (no-op) rather than corrupting state if n would overflow
    // capacity — callers must check this on every completion.
    [[nodiscard]] bool commit(std::size_t n) noexcept {
        if (size_ + n > data_.size()) return false;
        size_ += n;
        return true;
    }

    void clear() noexcept { size_ = 0; }

private:
    std::vector<std::byte> data_;
    std::size_t size_ = 0;
};

// Bounded pool of fixed-size ByteBuffers. A buffer checked out via acquire()
// must not be returned to the pool (release()) until every operation
// referencing it has completed — the caller (transport layer) owns that
// invariant, the pool only owns allocation/reuse.
class BufferPool {
public:
    BufferPool(std::size_t buffer_count, std::size_t buffer_size)
        : buffer_size_(buffer_size) {
        for (std::size_t i = 0; i < buffer_count; ++i) {
            free_list_.push_back(std::make_unique<ByteBuffer>(buffer_size));
        }
    }

    // Returns nullptr on exhaustion rather than allocating unboundedly —
    // callers must handle this (backpressure/reject), see
    // docs/rtmp_promot.md "no uncontrolled allocation per media packet".
    [[nodiscard]] std::unique_ptr<ByteBuffer> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.empty()) {
            ++exhausted_count_;
            return nullptr;
        }
        auto buf = std::move(free_list_.back());
        free_list_.pop_back();
        buf->clear();
        return buf;
    }

    void release(std::unique_ptr<ByteBuffer> buffer) {
        if (!buffer) return;
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push_back(std::move(buffer));
    }

    [[nodiscard]] std::size_t buffer_size() const noexcept { return buffer_size_; }
    [[nodiscard]] std::uint64_t exhausted_count() const noexcept { return exhausted_count_; }

private:
    std::size_t buffer_size_;
    std::mutex mutex_;
    std::deque<std::unique_ptr<ByteBuffer>> free_list_;
    std::atomic<std::uint64_t> exhausted_count_{0};
};

} // namespace rtmp_server::core
