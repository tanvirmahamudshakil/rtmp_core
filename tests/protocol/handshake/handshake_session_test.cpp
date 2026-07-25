#include "rtmp_server/protocol/handshake/handshake_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace rtmp_server::protocol::handshake {
namespace {

std::vector<std::byte> to_bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (int v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

std::vector<std::byte> make_c1(std::uint32_t time = 0) {
    std::vector<std::byte> c1;
    c1.reserve(kHandshakeChunkSize);
    c1.push_back(static_cast<std::byte>((time >> 24) & 0xFF));
    c1.push_back(static_cast<std::byte>((time >> 16) & 0xFF));
    c1.push_back(static_cast<std::byte>((time >> 8) & 0xFF));
    c1.push_back(static_cast<std::byte>(time & 0xFF));
    for (int i = 0; i < 4; ++i) c1.push_back(std::byte{0});
    for (std::size_t i = 0; i < kRandomEchoSize; ++i) {
        c1.push_back(static_cast<std::byte>(i % 256));
    }
    return c1;
}

// Test fixture that wires all three handlers and records what happened, so
// each test can assert on the observable outcome without duplicating the
// wiring boilerplate.
class HandshakeSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_.set_send_handler([this](core::SharedBuffer buf) { sent_buffers_.push_back(std::move(buf)); });
        session_.set_complete_handler([this]() { completed_ = true; });
        session_.set_fail_handler([this](core::Error err) {
            failed_ = true;
            last_error_ = err;
        });
    }

    HandshakeSession session_;
    std::vector<core::SharedBuffer> sent_buffers_;
    bool completed_ = false;
    bool failed_ = false;
    core::Error last_error_;
};

TEST_F(HandshakeSessionTest, InitialStateIsWaitingForC0) {
    EXPECT_EQ(session_.state(), HandshakeState::WaitingForC0);
}

TEST_F(HandshakeSessionTest, FullHandshakeInOneShotCompletes) {
    std::vector<std::byte> input;
    input.push_back(std::byte{kRtmpVersion}); // C0
    auto c1 = make_c1(12345);
    input.insert(input.end(), c1.begin(), c1.end()); // C1

    session_.on_bytes_received(input);

    // Server must have responded with S0+S1+S2 as soon as C1 arrived.
    ASSERT_EQ(sent_buffers_.size(), 1u);
    EXPECT_EQ(sent_buffers_[0].size(), kC0Size + 2 * kHandshakeChunkSize);
    EXPECT_EQ(session_.state(), HandshakeState::WaitingForC2);
    EXPECT_FALSE(completed_);

    // S0 byte is the RTMP version.
    EXPECT_EQ(static_cast<std::uint8_t>(sent_buffers_[0].view()[0]), kRtmpVersion);

    // S2 (tail of the response) must echo C1 verbatim.
    auto response = sent_buffers_[0].view();
    auto s2 = response.subspan(kC0Size + kHandshakeChunkSize, kHandshakeChunkSize);
    ASSERT_EQ(s2.size(), c1.size());
    EXPECT_TRUE(std::equal(s2.begin(), s2.end(), c1.begin()));

    std::vector<std::byte> c2(kHandshakeChunkSize, std::byte{0xAB});
    session_.on_bytes_received(c2);

    EXPECT_TRUE(completed_);
    EXPECT_FALSE(failed_);
    EXPECT_EQ(session_.state(), HandshakeState::Completed);
}

TEST_F(HandshakeSessionTest, FragmentedC0C1SucceedsByteAtATime) {
    std::vector<std::byte> input;
    input.push_back(std::byte{kRtmpVersion});
    auto c1 = make_c1();
    input.insert(input.end(), c1.begin(), c1.end());

    for (auto b : input) {
        session_.on_bytes_received(std::span<const std::byte>(&b, 1));
    }

    ASSERT_EQ(sent_buffers_.size(), 1u);
    EXPECT_EQ(session_.state(), HandshakeState::WaitingForC2);
    EXPECT_FALSE(failed_);
}

TEST_F(HandshakeSessionTest, C0AndC1DeliveredTogetherThenSplitAcrossReceives) {
    std::vector<std::byte> input;
    input.push_back(std::byte{kRtmpVersion});
    auto c1 = make_c1();
    input.insert(input.end(), c1.begin(), c1.end());

    // Split into three uneven chunks.
    session_.on_bytes_received(std::span(input).subspan(0, 500));
    session_.on_bytes_received(std::span(input).subspan(500, 500));
    session_.on_bytes_received(std::span(input).subspan(1000));

    ASSERT_EQ(sent_buffers_.size(), 1u);
    EXPECT_EQ(session_.state(), HandshakeState::WaitingForC2);
}

TEST_F(HandshakeSessionTest, FragmentedC2SucceedsInSmallPieces) {
    std::vector<std::byte> input;
    input.push_back(std::byte{kRtmpVersion});
    auto c1 = make_c1();
    input.insert(input.end(), c1.begin(), c1.end());
    session_.on_bytes_received(input);
    ASSERT_EQ(session_.state(), HandshakeState::WaitingForC2);

    std::vector<std::byte> c2(kHandshakeChunkSize, std::byte{0x11});
    for (std::size_t offset = 0; offset < c2.size(); offset += 7) {
        auto n = std::min<std::size_t>(7, c2.size() - offset);
        session_.on_bytes_received(std::span(c2).subspan(offset, n));
    }

    EXPECT_TRUE(completed_);
    EXPECT_EQ(session_.state(), HandshakeState::Completed);
}

TEST_F(HandshakeSessionTest, InvalidVersionIsRejected) {
    auto data = to_bytes({0x99});
    session_.on_bytes_received(data);

    EXPECT_TRUE(failed_);
    EXPECT_EQ(session_.state(), HandshakeState::Failed);
    EXPECT_EQ(last_error_.code(), core::ErrorCode::MalformedHandshake);
    EXPECT_TRUE(sent_buffers_.empty());
}

TEST_F(HandshakeSessionTest, OversizedHandshakeDataIsRejected) {
    std::vector<std::byte> flood(kMaxHandshakeBytes + 1, std::byte{0x03});
    session_.on_bytes_received(flood);

    EXPECT_TRUE(failed_);
    EXPECT_EQ(session_.state(), HandshakeState::Failed);
}

TEST_F(HandshakeSessionTest, TimeoutBeforeCompletionMarksTimedOut) {
    session_.on_bytes_received(to_bytes({kRtmpVersion}));
    EXPECT_EQ(session_.state(), HandshakeState::WaitingForC1);

    session_.on_timeout();

    EXPECT_EQ(session_.state(), HandshakeState::TimedOut);
    EXPECT_TRUE(failed_);
    EXPECT_EQ(last_error_.code(), core::ErrorCode::ConnectionTimedOut);
}

TEST_F(HandshakeSessionTest, TimeoutAfterCompletionIsNoOp) {
    std::vector<std::byte> input;
    input.push_back(std::byte{kRtmpVersion});
    auto c1 = make_c1();
    input.insert(input.end(), c1.begin(), c1.end());
    session_.on_bytes_received(input);
    session_.on_bytes_received(std::vector<std::byte>(kHandshakeChunkSize, std::byte{0}));
    ASSERT_TRUE(completed_);
    failed_ = false;

    session_.on_timeout();

    EXPECT_FALSE(failed_);
    EXPECT_EQ(session_.state(), HandshakeState::Completed);
}

TEST_F(HandshakeSessionTest, BytesAfterFailureAreIgnored) {
    session_.on_bytes_received(to_bytes({0x99})); // invalid version
    ASSERT_TRUE(failed_);
    sent_buffers_.clear();
    failed_ = false;

    session_.on_bytes_received(to_bytes({kRtmpVersion}));

    EXPECT_FALSE(failed_);
    EXPECT_TRUE(sent_buffers_.empty());
    EXPECT_EQ(session_.state(), HandshakeState::Failed);
}

} // namespace
} // namespace rtmp_server::protocol::handshake
