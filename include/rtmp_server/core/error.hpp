#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rtmp_server::core {

enum class ErrorCategory : std::uint8_t {
    Protocol,
    Network,
    Configuration,
    Authentication,
    Storage,
    Internal,
};

enum class ErrorCode : std::uint32_t {
    // Generic
    None = 0,
    Unknown,
    NotFound,
    Conflict,

    // Network
    ConnectionClosed,
    ConnectionReset,
    ConnectionTimedOut,
    WouldBlock,
    OperationCanceled,
    ResourceExhausted,

    // Protocol
    MalformedHandshake,
    MalformedChunk,
    MalformedAmf,
    MessageTooLarge,
    InvalidStateTransition,

    // Configuration
    InvalidConfiguration,
    MissingConfiguration,

    // Authentication
    InvalidStreamKey,
    ExpiredToken,
    Unauthorized,

    // Storage
    StorageUnavailable,
    StorageWriteFailed,

    // Caller/peer supplied a structurally invalid value (Phase 8). Distinct
    // from InvalidConfiguration, which means operator-supplied configuration
    // is wrong: this one means a client-controlled value failed validation,
    // e.g. a stream name that is unsafe to use as a filesystem path.
    // Appended at the end deliberately — no switch in the tree is exhaustive
    // over ErrorCode, but the numeric values appear in logs and metrics, so
    // existing codes must not be renumbered.
    InvalidArgument,
};

// Lightweight, allocation-free-in-the-common-case error value for hot-path
// packet processing. Deliberately not an exception: parsing untrusted RTMP
// input must reject malformed data without paying exception-unwind cost.
class Error {
public:
    Error() noexcept : code_(ErrorCode::None), category_(ErrorCategory::Internal) {}

    Error(ErrorCode code, ErrorCategory category, std::string_view message = {})
        : code_(code), category_(category), message_(message) {}

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::None; }
    explicit operator bool() const noexcept { return !ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] ErrorCategory category() const noexcept { return category_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    static Error none() noexcept { return Error{}; }

private:
    ErrorCode code_;
    ErrorCategory category_;
    std::string message_;
};

} // namespace rtmp_server::core
