#pragma once

#include <utility>
#include <variant>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::core {

// Result<T> avoids exceptions on expected failure paths (parsing, I/O)
// while still making "did this fail" impossible to ignore silently.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {} // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

// Specialization for operations that either succeed with no value or fail.
template <>
class Result<void> {
public:
    Result() : error_(Error::none()) {}
    Result(Error error) : error_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const& { return error_; }

private:
    Error error_;
};

} // namespace rtmp_server::core
