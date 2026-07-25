#include "rtmp_server/protocol/handshake/handshake_session.hpp"

#include <algorithm>
#include <array>

#include "rtmp_server/core/byte_order.hpp"
#include "rtmp_server/core/random.hpp"

namespace rtmp_server::protocol::handshake {

namespace {

void append_u32_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 4> tmp{};
    core::write_u32_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

} // namespace

HandshakeSession::HandshakeSession(core::MonotonicClock::time_point start_time)
    : start_time_(start_time) {}

void HandshakeSession::fail(core::ErrorCode code, core::ErrorCategory category, std::string_view message) {
    if (is_terminal()) return;
    state_ = HandshakeState::Failed;
    buffer_.clear();
    if (fail_handler_) fail_handler_(core::Error(code, category, message));
}

void HandshakeSession::on_timeout() {
    if (is_terminal()) return;
    state_ = HandshakeState::TimedOut;
    buffer_.clear();
    if (fail_handler_) {
        fail_handler_(core::Error(core::ErrorCode::ConnectionTimedOut, core::ErrorCategory::Network,
                                   "RTMP handshake did not complete before the configured deadline"));
    }
}

void HandshakeSession::on_bytes_received(std::span<const std::byte> data) {
    if (is_terminal() || data.empty()) return;

    // Bound the total amount of data this session will ever accumulate:
    // C0 + C1 + C2 is a fixed, known size. A peer sending more than that
    // before finishing the handshake is malformed (or hostile) input —
    // reject it rather than growing the buffer without limit
    // (docs/rtmp_promot.md "Security Requirements": handshake-size limits).
    if (buffer_.size() + data.size() > kMaxHandshakeBytes) {
        fail(core::ErrorCode::MalformedHandshake, core::ErrorCategory::Protocol,
             "handshake data exceeds the maximum C0+C1+C2 size");
        return;
    }

    buffer_.insert(buffer_.end(), data.begin(), data.end());

    if (state_ == HandshakeState::WaitingForC0) try_consume_c0();
    if (state_ == HandshakeState::WaitingForC1) try_consume_c1();
    if (state_ == HandshakeState::WaitingForC2) try_consume_c2();
}

void HandshakeSession::try_consume_c0() {
    if (buffer_.size() < kC0Size) return;

    auto version = static_cast<std::uint8_t>(buffer_[0]);
    if (version != kRtmpVersion) {
        fail(core::ErrorCode::MalformedHandshake, core::ErrorCategory::Protocol,
             "unsupported RTMP version in C0");
        return;
    }

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kC0Size));
    state_ = HandshakeState::WaitingForC1;
}

void HandshakeSession::try_consume_c1() {
    if (buffer_.size() < kHandshakeChunkSize) return;

    // C1's time field is retained verbatim (not parsed further — the simple
    // handshake does not require the server to interpret it) so S2 can echo
    // it back, along with C1's zero field and random payload, per
    // docs/rtmp_promot.md: "S2 = echo of C1".
    std::vector<std::byte> c1(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kHandshakeChunkSize));
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kHandshakeChunkSize));

    state_ = HandshakeState::SendingS0S1S2;

    // Build S1: server's own time (elapsed ms since this session started)
    // and zero field, followed by fresh cryptographically secure random
    // bytes (core/random.hpp), not derived from or equal to C1's random data.
    std::vector<std::byte> s1_random(kRandomEchoSize);
    core::secure_random_bytes(s1_random);

    std::vector<std::byte> out;
    out.reserve(kC0Size + 2 * kHandshakeChunkSize);

    // S0
    out.push_back(std::byte{kRtmpVersion});

    // S1
    append_u32_be(out, core::to_millis(core::monotonic_now() - start_time_));
    append_u32_be(out, 0);
    out.insert(out.end(), s1_random.begin(), s1_random.end());

    // S2 = echo of C1 verbatim (time field, zero/echo field, random bytes).
    out.insert(out.end(), c1.begin(), c1.end());

    if (send_handler_) send_handler_(core::SharedBuffer::adopt(std::move(out)));

    state_ = HandshakeState::WaitingForC2;
}

void HandshakeSession::try_consume_c2() {
    if (buffer_.size() < kHandshakeChunkSize) return;

    // C2 is expected to echo S1, but per the simple handshake many real
    // clients (and this server, symmetrically) do not require byte-exact
    // validation of the echoed content to interoperate — only its size is
    // enforced here. Bounds-checked, fixed-size consumption either way.
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kHandshakeChunkSize));

    state_ = HandshakeState::Completed;
    if (complete_handler_) complete_handler_();
}

} // namespace rtmp_server::protocol::handshake
