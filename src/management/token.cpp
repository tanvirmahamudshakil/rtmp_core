#include "rtmp_server/management/token.hpp"

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/core/hmac.hpp"

namespace rtmp_server::management {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

namespace {
std::string signing_input(std::string_view application, std::string_view name, std::int64_t expires_at_unix) {
    std::string out;
    out.reserve(application.size() + name.size() + 24);
    out += application;
    out += ':';
    out += name;
    out += ':';
    out += std::to_string(expires_at_unix);
    return out;
}
} // namespace

std::string sign_token(std::string_view secret, std::string_view application, std::string_view name,
                        std::int64_t expires_at_unix) {
    return core::hmac_sha256_hex(secret, signing_input(application, name, expires_at_unix));
}

Result<void> verify_token(std::string_view secret, std::string_view application, std::string_view name,
                           std::string_view token, std::int64_t expires_at_unix, std::int64_t now_unix) {
    std::string expected = sign_token(secret, application, name, expires_at_unix);
    if (!core::constant_time_equals(expected, token)) {
        return Error(ErrorCode::Unauthorized, ErrorCategory::Authentication, "invalid token signature");
    }
    if (now_unix > expires_at_unix) {
        return Error(ErrorCode::ExpiredToken, ErrorCategory::Authentication, "token expired");
    }
    return Result<void>{};
}

} // namespace rtmp_server::management
