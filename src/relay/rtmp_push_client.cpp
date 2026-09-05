#include "rtmp_server/relay/rtmp_push_client.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"

namespace rtmp_server::relay {
namespace {

using protocol::amf0::Amf0Value;
using protocol::chunk::MessageTypeId;
using protocol::chunk::RtmpMessage;

constexpr std::uint32_t kCommandChunkStream = 3;
// Separate chunk streams for audio and video, as every publisher does: it lets
// the target's decoder interleave them with per-type header compression rather
// than re-sending a full header on every alternation.
constexpr std::uint32_t kAudioChunkStream = 4;
constexpr std::uint32_t kVideoChunkStream = 6;
constexpr std::uint32_t kDataChunkStream = 5;
constexpr std::size_t kHandshakeResponseSize =
    protocol::handshake::kC0Size + 2 * protocol::handshake::kHandshakeChunkSize;

core::Error network_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Network, std::move(message));
}

std::uint16_t read_be16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint8_t>(bytes[offset + 1]));
}

void append_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

RtmpMessage command(std::uint32_t stream_id, const std::vector<Amf0Value>& values) {
    RtmpMessage message;
    message.chunk_stream_id = kCommandChunkStream;
    message.message_stream_id = stream_id;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& value : values) protocol::amf0::encode(value, message.payload);
    return message;
}

std::span<const std::byte> command_payload(const RtmpMessage& message) {
    if (message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Amf3Command)) {
        if (message.payload.empty() || message.payload.front() != std::byte{0}) return {};
        return std::span<const std::byte>(message.payload).subspan(1);
    }
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    return message.payload;
}

std::string status_code(const std::vector<Amf0Value>& values) {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        if (const auto* code = it->find("code"); code != nullptr && code->is_string()) {
            return code->as_string();
        }
    }
    return {};
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

RtmpPushClient::RtmpPushClient(std::string target_url)
    : RtmpPushClient(std::move(target_url), Options{}) {}

RtmpPushClient::RtmpPushClient(std::string target_url, Options options)
    : target_url_(std::move(target_url)), options_(options), encoder_(options_.chunk_size) {}

RtmpPushClient::~RtmpPushClient() {
    if (fd_ >= 0) ::close(fd_);
}

core::Result<int> RtmpPushClient::connect_socket(const protocol::RtmpUrl& parsed,
                                                 const ContinuePredicate& should_continue) const {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(parsed.port);
    const int gai = ::getaddrinfo(parsed.host.c_str(), service.c_str(), &hints, &addresses);
    if (gai != 0) {
        return network_error(core::ErrorCode::NotFound,
                             "RTMP target DNS lookup failed: " + std::string(gai_strerror(gai)));
    }

    const auto deadline = std::chrono::steady_clock::now() + options_.connect_timeout;
    std::string last_error = "no address could be connected";
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        if (!should_continue()) break;
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }
        if (!set_nonblocking(fd)) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc == 0) {
            ::freeaddrinfo(addresses);
            return fd;
        }
        if (errno != EINPROGRESS && errno != EINTR) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }
        while (should_continue() && std::chrono::steady_clock::now() < deadline) {
            pollfd item{fd, POLLOUT, 0};
            const int polled = ::poll(&item, 1, 250);
            if (polled < 0 && errno == EINTR) continue;
            if (polled <= 0) continue;
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                socket_error == 0) {
                ::freeaddrinfo(addresses);
                return fd;
            }
            last_error = std::strerror(socket_error == 0 ? errno : socket_error);
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    if (!should_continue()) {
        return network_error(core::ErrorCode::OperationCanceled, "RTMP target push stopped");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        return network_error(core::ErrorCode::ConnectionTimedOut, "RTMP target connection timed out");
    }
    return network_error(core::ErrorCode::ConnectionClosed,
                         "RTMP target connection failed: " + last_error);
}

void RtmpPushClient::fail(core::ErrorCode code, core::ErrorCategory category, std::string message) {
    if (error_) return;
    error_.emplace(code, category, std::move(message));
    state_ = State::Failed;
}

void RtmpPushClient::queue_bytes(std::span<const std::byte> bytes) {
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void RtmpPushClient::queue_message(const RtmpMessage& message) {
    std::vector<std::byte> encoded;
    encoder_.encode_message(message, encoded);
    queue_bytes(encoded);
}

bool RtmpPushClient::drain_output() {
    while (output_offset_ < output_.size()) {
        const auto remaining = output_.size() - output_offset_;
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto sent = ::send(fd_, output_.data() + output_offset_, remaining, flags);
        if (sent > 0) {
            output_offset_ += static_cast<std::size_t>(sent);
            bytes_sent_ += static_cast<std::uint64_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        // The target is not reading fast enough. Leave the tail queued and come
        // back on the next POLLOUT; the queue in front of this client is what
        // bounds how far behind a slow target may fall.
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
             "RTMP target send failed: " + std::string(std::strerror(errno)));
        return false;
    }
    output_.clear();
    output_offset_ = 0;
    return true;
}

void RtmpPushClient::send_connect(const protocol::RtmpUrl& parsed) {
    queue_message(command(0, {Amf0Value::string("connect"), Amf0Value::number(transaction_id_++),
                              Amf0Value::object({
                                  {"app", Amf0Value::string(parsed.application)},
                                  {"type", Amf0Value::string("nonprivate")},
                                  {"flashVer", Amf0Value::string("FMLE/3.0 (compatible; StreamForge)")},
                                  {"tcUrl", Amf0Value::string(parsed.tc_url)},
                                  {"fpad", Amf0Value::boolean(false)},
                                  {"capabilities", Amf0Value::number(239)},
                                  {"audioCodecs", Amf0Value::number(3575)},
                                  {"videoCodecs", Amf0Value::number(252)},
                                  {"videoFunction", Amf0Value::number(1)},
                                  {"objectEncoding", Amf0Value::number(0)},
                              })}));
    state_ = State::Connecting;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpPushClient::send_publish_preamble(const protocol::RtmpUrl& parsed) {
    // FMLE's publish preamble, then createStream. releaseStream and FCPublish
    // are fire-and-forget: the large CDN ingests reject or silently stall a
    // publisher that skips them, and no target minds receiving them, so they
    // are sent unconditionally and only createStream's result is awaited.
    queue_message(command(0, {Amf0Value::string("releaseStream"),
                              Amf0Value::number(transaction_id_++), Amf0Value::null(),
                              Amf0Value::string(parsed.stream)}));
    queue_message(command(0, {Amf0Value::string("FCPublish"), Amf0Value::number(transaction_id_++),
                              Amf0Value::null(), Amf0Value::string(parsed.stream)}));
    queue_message(command(0, {Amf0Value::string("createStream"),
                              Amf0Value::number(transaction_id_++), Amf0Value::null()}));
    state_ = State::CreatingStream;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpPushClient::send_publish(const protocol::RtmpUrl& parsed) {
    queue_message(command(message_stream_id_,
                          {Amf0Value::string("publish"), Amf0Value::number(transaction_id_++),
                           Amf0Value::null(), Amf0Value::string(parsed.stream),
                           Amf0Value::string("live")}));
    state_ = State::Publishing;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpPushClient::handle_command(const RtmpMessage& message) {
    const auto payload = command_payload(message);
    auto decoded = protocol::amf0::decode_all(payload);
    if (!decoded || decoded.value().empty() || !decoded.value()[0].is_string()) return;
    const auto& values = decoded.value();
    const std::string& name = values[0].as_string();

    if (name == "_error") {
        fail(core::ErrorCode::Unauthorized, core::ErrorCategory::Authentication,
             "RTMP target rejected a command (bad stream key or application?)");
        return;
    }
    if (state_ == State::Connecting && name == "_result") {
        state_ = State::PreamblePending;
        return;
    }
    if (state_ == State::CreatingStream && name == "_result") {
        if (values.size() >= 4 && values[3].is_number() && values[3].as_number() > 0) {
            message_stream_id_ = static_cast<std::uint32_t>(values[3].as_number());
        }
        if (message_stream_id_ == 0) {
            fail(core::ErrorCode::MalformedAmf, core::ErrorCategory::Protocol,
                 "RTMP createStream response has no stream id");
        }
        return;
    }
    if (name != "onStatus") return;

    const std::string code = status_code(values);
    if (code == "NetStream.Publish.Start") {
        state_ = State::Streaming;
        last_progress_ = std::chrono::steady_clock::now();
        return;
    }
    if (code.find("Failed") != std::string::npos || code.find("Rejected") != std::string::npos ||
        code.find("BadName") != std::string::npos || code == "NetStream.Publish.Denied" ||
        code == "NetStream.Unpublish.Success") {
        fail(core::ErrorCode::Unauthorized, core::ErrorCategory::Network,
             "RTMP target refused the publish: " + (code.empty() ? std::string("unknown status") : code));
    }
}

void RtmpPushClient::handle_user_control(const RtmpMessage& message) {
    if (message.payload.size() < 6 || read_be16(message.payload, 0) != 6) return; // PingRequest
    RtmpMessage response;
    response.chunk_stream_id = 2;
    response.message_stream_id = 0;
    response.message_type_id = static_cast<std::uint8_t>(MessageTypeId::UserControlMessage);
    append_be16(response.payload, 7); // PingResponse
    response.payload.insert(response.payload.end(), message.payload.begin() + 2,
                            message.payload.begin() + 6);
    queue_message(response);
}

void RtmpPushClient::handle_message(RtmpMessage message) {
    const auto type = static_cast<MessageTypeId>(message.message_type_id);
    // Anything the target sends is a sign of life; a publish has no other
    // liveness signal.
    last_progress_ = std::chrono::steady_clock::now();
    if (type == MessageTypeId::Amf0Command || type == MessageTypeId::Amf3Command) {
        handle_command(message);
    } else if (type == MessageTypeId::UserControlMessage) {
        handle_user_control(message);
    }
}

void RtmpPushClient::send_media(const media::HandoffMessage& source) {
    const auto absolute = clock_.unwrap(source.timestamp);
    if (!timestamp_base_) timestamp_base_ = absolute;
    // A target must see a stream that starts near zero: this server may have
    // been publishing for days before the target was added, and an RTMP
    // timestamp that starts in the hundreds of millions is rejected or
    // mis-buffered by several ingests.
    const auto relative = absolute >= *timestamp_base_ ? absolute - *timestamp_base_ : 0;

    RtmpMessage message;
    message.message_stream_id = message_stream_id_;
    message.timestamp = static_cast<std::uint32_t>(relative & 0xFFFFFFFFull);
    if (source.video) {
        message.chunk_stream_id = kVideoChunkStream;
        message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
    } else if (source.metadata) {
        message.chunk_stream_id = kDataChunkStream;
        message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    } else {
        message.chunk_stream_id = kAudioChunkStream;
        message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Audio);
    }
    message.payload = source.payload;
    queue_message(message);
}

core::Result<void> RtmpPushClient::run(const ContinuePredicate& should_continue,
                                       media::MediaHandoffQueue& queue,
                                       const PrimingProvider& priming,
                                       const PublishingHandler& on_publishing) {
    auto parsed = protocol::parse_rtmp_url(target_url_);
    if (!parsed) return parsed.error();
    auto connected = connect_socket(parsed.value(), should_continue);
    if (!connected) return connected.error();
    fd_ = connected.value();

    error_.reset();
    output_.clear();
    output_offset_ = 0;
    message_stream_id_ = 0;
    transaction_id_ = 1.0;
    bytes_sent_ = 0;
    timestamp_base_.reset();
    clock_ = media::TimestampUnwrapper{};
    bool primed = false;
    // A target only becomes decodable from a keyframe; everything before the
    // first one after (re)connect would be undecodable reference frames.
    bool awaiting_keyframe = true;

    decoder_.emplace(options_.max_message_size);
    decoder_->set_message_handler([this](RtmpMessage message) { handle_message(std::move(message)); });
    decoder_->set_error_handler(
        [this](core::Error error) { fail(error.code(), error.category(), error.message()); });

    std::vector<std::byte> c0c1;
    c0c1.reserve(1 + protocol::handshake::kHandshakeChunkSize);
    c0c1.push_back(static_cast<std::byte>(protocol::handshake::kRtmpVersion));
    c0c1.resize(1 + protocol::handshake::kHandshakeChunkSize, std::byte{0});
    for (std::size_t i = 9; i < c0c1.size(); ++i) {
        c0c1[i] = static_cast<std::byte>((i * 31u + 7u) & 0xffu);
    }
    queue_bytes(c0c1);
    state_ = State::Handshaking;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
    last_progress_ = std::chrono::steady_clock::now();

    std::array<std::byte, 32 * 1024> input{};
    while (should_continue() && !error_) {
        const auto now = std::chrono::steady_clock::now();
        if (state_ != State::Streaming && now >= state_deadline_) {
            fail(core::ErrorCode::ConnectionTimedOut, core::ErrorCategory::Network,
                 "RTMP target handshake/publish handshake timed out");
            break;
        }
        if (state_ == State::Streaming && now - last_progress_ >= options_.idle_timeout) {
            fail(core::ErrorCode::ConnectionTimedOut, core::ErrorCategory::Network,
                 "RTMP target stopped acknowledging");
            break;
        }

        if (state_ == State::Streaming) {
            if (!primed) {
                for (const auto& message : priming()) send_media(message);
                primed = true;
                if (on_publishing) on_publishing();
            }
            // Bounded wait: the socket still has to be serviced for
            // acknowledgements, pings and a target that drops the connection.
            media::HandoffMessage message;
            while (queue.pop_for(message, std::chrono::milliseconds(20))) {
                if (awaiting_keyframe) {
                    if (message.video && !message.keyframe && !message.sequence_header) continue;
                    if (message.video && message.keyframe) awaiting_keyframe = false;
                }
                send_media(message);
                if (output_.size() - output_offset_ > 4u * 1024u * 1024u) break;
            }
        }

        short events = POLLIN;
        if (output_offset_ < output_.size()) events = static_cast<short>(events | POLLOUT);
        pollfd item{fd_, events, 0};
        const int polled = ::poll(&item, 1, state_ == State::Streaming ? 5 : 250);
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) {
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP target poll failed: " + std::string(std::strerror(errno)));
            break;
        }
        if ((item.revents & POLLOUT) != 0 && !drain_output()) break;
        if ((item.revents & (POLLERR | POLLNVAL)) != 0) {
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP target socket reported a transport error");
            break;
        }
        if ((item.revents & (POLLIN | POLLHUP)) == 0) continue;

        for (;;) {
            const auto received = ::recv(fd_, input.data(), input.size(), 0);
            if (received > 0) {
                std::span<const std::byte> bytes(input.data(), static_cast<std::size_t>(received));
                if (state_ == State::Handshaking) {
                    handshake_input_.insert(handshake_input_.end(), bytes.begin(), bytes.end());
                    if (handshake_input_.size() >= kHandshakeResponseSize) {
                        if (handshake_input_[0] !=
                            static_cast<std::byte>(protocol::handshake::kRtmpVersion)) {
                            fail(core::ErrorCode::MalformedHandshake, core::ErrorCategory::Protocol,
                                 "RTMP target returned an unsupported handshake version");
                            break;
                        }
                        // C2 is an exact echo of S1.
                        queue_bytes(std::span<const std::byte>(handshake_input_)
                                        .subspan(1, protocol::handshake::kHandshakeChunkSize));
                        std::vector<std::byte> set_chunk_size;
                        encoder_.encode_set_chunk_size(options_.chunk_size, set_chunk_size);
                        queue_bytes(set_chunk_size);
                        send_connect(parsed.value());
                        const auto trailing = std::span<const std::byte>(handshake_input_)
                                                  .subspan(kHandshakeResponseSize);
                        if (!trailing.empty()) decoder_->on_bytes_received(trailing);
                        handshake_input_.clear();
                    }
                } else {
                    decoder_->on_bytes_received(bytes);
                }
                if (decoder_ && decoder_->acknowledgement_due()) {
                    std::vector<std::byte> ack;
                    encoder_.encode_acknowledgement(
                        static_cast<std::uint32_t>(decoder_->bytes_received()), ack);
                    queue_bytes(ack);
                    decoder_->mark_acknowledged();
                }
                if (state_ == State::PreamblePending) {
                    send_publish_preamble(parsed.value());
                }
                if (state_ == State::CreatingStream && message_stream_id_ != 0) {
                    send_publish(parsed.value());
                }
                if (error_) break;
                continue;
            }
            if (received == 0) {
                fail(core::ErrorCode::ConnectionClosed, core::ErrorCategory::Network,
                     "RTMP target closed the connection");
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP target receive failed: " + std::string(std::strerror(errno)));
            break;
        }

        if (!error_ && output_offset_ < output_.size() && !drain_output()) break;
    }

    ::close(fd_);
    fd_ = -1;
    if (error_) return *error_;
    return {};
}

} // namespace rtmp_server::relay
