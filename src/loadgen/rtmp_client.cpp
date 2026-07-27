#include "rtmp_server/loadgen/rtmp_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"

namespace rtmp_server::loadgen {
namespace {

using protocol::amf0::Amf0Value;
using protocol::chunk::MessageTypeId;
using protocol::chunk::RtmpMessage;

constexpr std::size_t kS0S1S2Size =
    protocol::handshake::kC0Size + 2 * protocol::handshake::kHandshakeChunkSize;

// Chunk stream IDs, following the de-facto convention every RTMP client uses:
// 3 for commands, 4 for audio, 6 for video. Distinct IDs matter because the
// server's decoder keeps independent per-chunk-stream header state, so
// sharing one ID for commands and media would exercise a path no real
// encoder produces.
constexpr std::uint32_t kCommandChunkStream = 3;
constexpr std::uint32_t kAudioChunkStream = 4;
constexpr std::uint32_t kVideoChunkStream = 6;

// Writing to a socket whose peer has gone away raises SIGPIPE by default,
// which terminates the process. A load generator disconnects peers
// constantly, so this must be suppressed. The mechanism is platform-split:
// BSD/Darwin use the SO_NOSIGPIPE socket option, Linux uses the MSG_NOSIGNAL
// send flag (kSendFlags below).
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void suppress_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

RtmpMessage make_command(std::uint32_t message_stream_id, const std::vector<Amf0Value>& values) {
    RtmpMessage msg;
    msg.chunk_stream_id = kCommandChunkStream;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& v : values) protocol::amf0::encode(v, msg.payload);
    return msg;
}

// The first AMF0 string of a command message, or "" if it is not a command.
std::string command_name(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    auto decoded = protocol::amf0::decode_all(message.payload);
    if (!decoded.ok() || decoded.value().empty()) return {};
    const auto& name = decoded.value()[0];
    return name.is_string() ? name.as_string() : std::string{};
}

} // namespace

RtmpClient::RtmpClient(Config config)
    : config_(std::move(config)), encoder_(config_.chunk_size) {
    if (config_.role == Role::Publisher) {
        media_ = std::make_unique<MediaSource>(config_.media);
    }
    decoder_ = std::make_unique<protocol::chunk::ChunkDecoder>(config_.max_message_size);
    decoder_->set_message_handler([this](const RtmpMessage& message) { handle_message(message); });
    decoder_->set_error_handler([this](core::Error error) { fail(std::string(error.message())); });
}

RtmpClient::~RtmpClient() {
    if (fd_ >= 0) ::close(fd_);
}

void RtmpClient::fail(std::string reason) {
    if (state_ == State::Failed) return;
    stats_.failure_reason = std::move(reason);
    state_ = State::Failed;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

core::Result<void> RtmpClient::start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        fail(std::string("socket: ") + std::strerror(errno));
        return core::Error(core::ErrorCode::ConnectionClosed, core::ErrorCategory::Network, "socket() failed");
    }
    if (!set_nonblocking(fd_)) {
        fail("fcntl O_NONBLOCK failed");
        return core::Error(core::ErrorCode::ConnectionClosed, core::ErrorCategory::Network, "fcntl() failed");
    }
    // Media is latency-sensitive and our writes are already batched per tick;
    // Nagle would only add delay and distort the latency numbers we report.
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    suppress_sigpipe(fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        fail("inet_pton: invalid host address");
        return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Network, "bad host");
    }

    connect_started_ = Clock::now();
    const int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        finish_tcp_connect();
        return {};
    }
    if (errno == EINPROGRESS || errno == EINTR) {
        state_ = State::Connecting; // completion reported via writability
        return {};
    }
    fail(std::string("connect: ") + std::strerror(errno));
    return core::Error(core::ErrorCode::ConnectionClosed, core::ErrorCategory::Network, "connect() failed");
}

bool RtmpClient::wants_write() const noexcept {
    if (state_ == State::Connecting) return true; // connect completion
    return out_offset_ < out_buffer_.size();
}

void RtmpClient::queue_out(std::span<const std::byte> bytes) {
    out_buffer_.insert(out_buffer_.end(), bytes.begin(), bytes.end());
}

void RtmpClient::queue_message(const RtmpMessage& message) {
    std::vector<std::byte> encoded;
    encoder_.encode_message(message, encoded);
    queue_out(encoded);
}

void RtmpClient::drain_out() {
    while (out_offset_ < out_buffer_.size()) {
        const std::size_t remaining = out_buffer_.size() - out_offset_;
        const ssize_t n = ::send(fd_, out_buffer_.data() + out_offset_, remaining, kSendFlags);
        if (n > 0) {
            out_offset_ += static_cast<std::size_t>(n);
            stats_.bytes_sent += static_cast<std::uint64_t>(n);
            // A short write is normal TCP behaviour, not an error — counting
            // it is one of the things Phase 7 asks the tool to observe.
            if (static_cast<std::size_t>(n) < remaining) ++stats_.partial_writes;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; // retry on next writable
        if (n < 0 && errno == EINTR) continue;
        fail(std::string("send: ") + std::strerror(errno == 0 ? EPIPE : errno));
        return;
    }

    // Reclaim the drained prefix once it dominates, so a long-lived publisher
    // does not grow out_buffer_ forever. Compacting only when the drained
    // prefix is at least half the buffer keeps this amortised O(1).
    if (out_offset_ > 0 && out_offset_ * 2 >= out_buffer_.size()) {
        out_buffer_.erase(out_buffer_.begin(), out_buffer_.begin() + static_cast<std::ptrdiff_t>(out_offset_));
        out_offset_ = 0;
    }
}

void RtmpClient::finish_tcp_connect() {
    stats_.tcp_connect_latency =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - connect_started_);
    send_c0c1();
}

void RtmpClient::send_c0c1() {
    handshake_started_ = Clock::now();

    std::vector<std::byte> c0c1;
    c0c1.reserve(protocol::handshake::kC0Size + protocol::handshake::kHandshakeChunkSize);
    c0c1.push_back(static_cast<std::byte>(protocol::handshake::kRtmpVersion));

    // C1: 4-byte time (zero), 4-byte zero, then random echo bytes. A simple
    // deterministic filler is sufficient — this is the plain (non-"complex",
    // non-HMAC-signed) handshake the server implements.
    c1_sent_.assign(protocol::handshake::kHandshakeChunkSize, std::byte{0});
    for (std::size_t i = 8; i < c1_sent_.size(); ++i) {
        c1_sent_[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
    }
    c0c1.insert(c0c1.end(), c1_sent_.begin(), c1_sent_.end());

    queue_out(c0c1);
    state_ = State::Handshaking;
    drain_out();
}

void RtmpClient::on_handshake_bytes(std::span<const std::byte> data) {
    handshake_in_.insert(handshake_in_.end(), data.begin(), data.end());
    if (handshake_in_.size() < kS0S1S2Size) return;

    if (static_cast<std::uint8_t>(handshake_in_[0]) != protocol::handshake::kRtmpVersion) {
        fail("server sent an unsupported RTMP version in S0");
        return;
    }

    // Bytes beyond S0/S1/S2 are already RTMP chunk data (a server is entitled
    // to pipeline them); they must be fed to the decoder, not discarded —
    // this is the mirror image of the C2-adjacent-`connect` bug that
    // docs/production-gap-analysis.md item #3 records on the server side.
    std::vector<std::byte> trailing(handshake_in_.begin() + static_cast<std::ptrdiff_t>(kS0S1S2Size),
                                    handshake_in_.end());
    handshake_in_.clear();

    stats_.handshake_latency =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - handshake_started_);

    send_c2_and_connect();
    if (!trailing.empty() && state_ != State::Failed) decoder_->on_bytes_received(trailing);
}

void RtmpClient::send_c2_and_connect() {
    // C2 echoes S1. We kept only our own C1, and this server does not
    // validate the echo's contents (see HandshakeSession), so echo C1 back —
    // byte-count-correct and accepted, which is what the transport needs.
    queue_out(c1_sent_);

    // Advertise our chunk size before any large message, so the server's
    // decoder does not keep assuming the 128-byte default for our media.
    std::vector<std::byte> set_chunk;
    encoder_.encode_set_chunk_size(config_.chunk_size, set_chunk);
    queue_out(set_chunk);

    connect_command_started_ = Clock::now();
    const std::string tc_url = "rtmp://" + config_.host + "/" + config_.application;
    queue_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(transaction_id_++),
                                   Amf0Value::object({{"app", Amf0Value::string(config_.application)},
                                                      {"type", Amf0Value::string("nonprivate")},
                                                      {"flashVer", Amf0Value::string("FMLE/3.0 (rtmp_load_gen)")},
                                                      {"tcUrl", Amf0Value::string(tc_url)}})}));
    state_ = State::Connecting_Rtmp;
    drain_out();
}

void RtmpClient::send_create_stream() {
    queue_message(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(transaction_id_++),
                                   Amf0Value::null()}));
    state_ = State::CreatingStream;
    drain_out();
}

void RtmpClient::send_publish_or_play() {
    start_command_started_ = Clock::now();
    if (config_.role == Role::Publisher) {
        queue_message(make_command(message_stream_id_,
                                   {Amf0Value::string("publish"), Amf0Value::number(transaction_id_++),
                                    Amf0Value::null(), Amf0Value::string(config_.stream_key),
                                    Amf0Value::string("live")}));
    } else {
        queue_message(make_command(message_stream_id_,
                                   {Amf0Value::string("play"), Amf0Value::number(transaction_id_++),
                                    Amf0Value::null(), Amf0Value::string(config_.stream_key)}));
    }
    state_ = State::Starting;
    drain_out();
}

void RtmpClient::handle_message(const RtmpMessage& message) {
    const auto type = static_cast<MessageTypeId>(message.message_type_id);

    // Media arriving at a viewer.
    if (type == MessageTypeId::Video || type == MessageTypeId::Audio) {
        if (stats_.media_messages_received == 0 && streaming_started_ != Clock::time_point{}) {
            stats_.first_media_latency =
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - streaming_started_);
        }
        ++stats_.media_messages_received;
        if (type == MessageTypeId::Video) {
            ++stats_.video_messages_received;
            if (!message.payload.empty() && static_cast<std::uint8_t>(message.payload[0]) == 0x17 &&
                message.payload.size() > 1 && static_cast<std::uint8_t>(message.payload[1]) == 0x01) {
                ++stats_.keyframes_received;
            }
        } else {
            ++stats_.audio_messages_received;
        }

        // Byte-level corruption check against the generator's pattern. This
        // is what lets the tool claim "no corruption" rather than merely
        // "the right number of bytes arrived".
        ++stats_.payloads_verified;
        if (!MediaSource::verify_pattern(message.payload)) ++stats_.payloads_corrupt;
        return;
    }

    const std::string name = command_name(message);
    if (name.empty()) return; // control messages (ack, set chunk size, ...) need no action here

    if (name == "_error") {
        fail("server rejected a command with _error");
        return;
    }

    switch (state_) {
        case State::Connecting_Rtmp:
            if (name == "_result") {
                stats_.connect_command_latency =
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - connect_command_started_);
                send_create_stream();
            }
            break;

        case State::CreatingStream:
            if (name == "_result") {
                // createStream's _result carries the new message stream ID as
                // the 4th AMF0 value: [_result, txn, null, streamId].
                auto decoded = protocol::amf0::decode_all(message.payload);
                if (decoded.ok() && decoded.value().size() >= 4) {
                    if (decoded.value()[3].is_number()) {
                        message_stream_id_ = static_cast<std::uint32_t>(decoded.value()[3].as_number());
                    }
                }
                if (message_stream_id_ == 0) {
                    fail("createStream _result did not carry a usable stream id");
                    return;
                }
                send_publish_or_play();
            }
            break;

        case State::Starting:
            if (name == "onStatus" || name == "_result") {
                stats_.publish_or_play_latency =
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_command_started_);
                state_ = State::Streaming;
                stats_.reached_streaming = true;
                streaming_started_ = Clock::now();
            }
            break;

        default:
            break;
    }
}

void RtmpClient::on_readable() {
    if (fd_ < 0 || finished()) return;

    std::array<std::byte, 64 * 1024> buf{};
    std::size_t read_this_tick = 0;

    for (;;) {
        // Slow-viewer simulation: stop reading once the per-tick budget is
        // spent, leaving data in the kernel receive buffer. The server's
        // socket send buffer then fills, its per-viewer outbound queue grows,
        // and the real slow-viewer policy engages.
        std::size_t want = buf.size();
        if (config_.read_budget_per_tick != 0) {
            if (read_this_tick >= config_.read_budget_per_tick) break;
            want = std::min(want, config_.read_budget_per_tick - read_this_tick);
        }

        const ssize_t n = ::recv(fd_, buf.data(), want, 0);
        if (n > 0) {
            const auto count = static_cast<std::size_t>(n);
            read_this_tick += count;
            stats_.bytes_received += count;
            std::span<const std::byte> data(buf.data(), count);
            if (state_ == State::Handshaking) {
                on_handshake_bytes(data);
            } else if (state_ != State::Failed && state_ != State::Closed) {
                decoder_->on_bytes_received(data);
            }
            if (finished()) return;
            continue;
        }
        if (n == 0) {
            // Orderly peer close.
            state_ = State::Closed;
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        fail(std::string("recv: ") + std::strerror(errno));
        return;
    }
}

void RtmpClient::on_writable() {
    if (fd_ < 0 || finished()) return;

    if (state_ == State::Connecting) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
            fail(std::string("connect: ") + std::strerror(error != 0 ? error : errno));
            return;
        }
        finish_tcp_connect();
        return;
    }

    drain_out();
}

void RtmpClient::pump_media(std::uint32_t stream_time_ms) {
    if (config_.role != Role::Publisher || state_ != State::Streaming || media_ == nullptr) return;

    // Respect the bounded output budget: an encoder whose socket is backed up
    // stops producing rather than buffering without limit.
    const std::size_t pending = out_buffer_.size() - out_offset_;
    if (pending >= config_.max_pending_out_bytes) return;

    const auto send_frame = [this](const GeneratedFrame& frame) {
        RtmpMessage msg;
        const bool video = is_video(frame.kind);
        msg.chunk_stream_id = video ? kVideoChunkStream : kAudioChunkStream;
        msg.message_stream_id = message_stream_id_;
        msg.message_type_id =
            static_cast<std::uint8_t>(video ? MessageTypeId::Video : MessageTypeId::Audio);
        msg.timestamp = frame.timestamp_ms;
        msg.payload = frame.payload;
        queue_message(msg);
        ++stats_.media_messages_sent;
    };

    if (!sent_sequence_headers_) {
        // Sequence headers must precede any media, or a viewer joining later
        // gets frames it cannot initialise a decoder for.
        send_frame(media_->video_sequence_header());
        send_frame(media_->audio_sequence_header());
        sent_sequence_headers_ = true;
    }

    for (const auto& frame : media_->frames_until(stream_time_ms)) send_frame(frame);
    drain_out();
}

void RtmpClient::abort_connection() {
    // No deleteStream, no FIN-with-teardown: just drop the socket. Using
    // SO_LINGER with a zero timeout makes close() emit RST rather than a
    // graceful FIN, which is the harsher and more realistic failure the
    // server must survive.
    if (fd_ >= 0) {
        linger lin{};
        lin.l_onoff = 1;
        lin.l_linger = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        ::close(fd_);
        fd_ = -1;
    }
    state_ = State::Closed;
}

core::Result<void> RtmpClient::reconnect() {
    const auto carried = stats_; // preserve cumulative counters across the reconnect
    abort_connection();

    out_buffer_.clear();
    out_offset_ = 0;
    handshake_in_.clear();
    c1_sent_.clear();
    message_stream_id_ = 0;
    transaction_id_ = 1.0;
    sent_sequence_headers_ = false;
    streaming_started_ = Clock::time_point{};
    encoder_ = protocol::chunk::ChunkEncoder(config_.chunk_size);
    decoder_ = std::make_unique<protocol::chunk::ChunkDecoder>(config_.max_message_size);
    decoder_->set_message_handler([this](const RtmpMessage& message) { handle_message(message); });
    decoder_->set_error_handler([this](core::Error error) { fail(std::string(error.message())); });
    if (media_) media_->restart();

    stats_ = carried;
    ++stats_.reconnects;

    return start();
}

void RtmpClient::close_gracefully() {
    if (fd_ >= 0 && state_ == State::Streaming) {
        queue_message(make_command(message_stream_id_, {Amf0Value::string("deleteStream"),
                                                        Amf0Value::number(transaction_id_++), Amf0Value::null(),
                                                        Amf0Value::number(message_stream_id_)}));
        drain_out();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    state_ = State::Closed;
}

} // namespace rtmp_server::loadgen
