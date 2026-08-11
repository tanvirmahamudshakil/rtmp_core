#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"

namespace rtmp_server::transcoding::native {
namespace {

using protocol::amf0::Amf0Value;
using protocol::chunk::MessageTypeId;
using protocol::chunk::RtmpMessage;

constexpr std::uint32_t kCommandChunkStream = 3;
constexpr std::size_t kHandshakeResponseSize =
    protocol::handshake::kC0Size + 2 * protocol::handshake::kHandshakeChunkSize;

core::Error url_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration,
                       core::ErrorCategory::Configuration, std::move(message));
}

core::Error network_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Network, std::move(message));
}

std::uint16_t read_be16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint8_t>(bytes[offset + 1]));
}

std::uint32_t read_be24(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void append_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

void append_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
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

core::Result<RtmpSourceUrl> parse_rtmp_source_url(std::string_view url) {
    constexpr std::string_view scheme = "rtmp://";
    if (url.find('#') != std::string_view::npos) {
        return url_error("RTMP source URL fragments are not sent to the origin");
    }
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20u) {
            return url_error("RTMP source URL contains whitespace or a control character");
        }
    }
    if (!url.starts_with(scheme)) {
        return url_error("source URL must use plain rtmp:// (RTMPS is not enabled in the native client)");
    }
    const auto remainder = url.substr(scheme.size());
    const auto slash = remainder.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= remainder.size()) {
        return url_error("RTMP source URL must be rtmp://host[:port]/application/stream");
    }

    const auto authority = remainder.substr(0, slash);
    if (authority.find('@') != std::string_view::npos) {
        return url_error("userinfo in an RTMP URL is unsupported; pass authentication in the stream query");
    }

    std::string_view host;
    std::string_view port_text;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) return url_error("invalid bracketed IPv6 host");
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return url_error("invalid RTMP authority");
            port_text = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) return url_error("IPv6 RTMP hosts must be enclosed in brackets");
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) return url_error("RTMP source URL has an empty host");

    std::uint16_t port = 1935;
    if (!port_text.empty()) {
        unsigned parsed_port = 0;
        const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
        if (ec != std::errc{} || end != port_text.data() + port_text.size() || parsed_port == 0 ||
            parsed_port > std::numeric_limits<std::uint16_t>::max()) {
            return url_error("RTMP source URL has an invalid port");
        }
        port = static_cast<std::uint16_t>(parsed_port);
    } else if (authority.ends_with(':')) {
        return url_error("RTMP source URL has an empty port");
    }

    const auto path_and_query = remainder.substr(slash + 1);
    const auto query = path_and_query.find('?');
    const auto path = path_and_query.substr(0, query);
    const auto app_slash = path.find('/');
    if (app_slash == std::string_view::npos || app_slash == 0 || app_slash + 1 >= path.size()) {
        return url_error("RTMP source URL must contain both application and stream names");
    }

    RtmpSourceUrl parsed;
    parsed.host = std::string(host);
    parsed.port = port;
    parsed.application = std::string(path.substr(0, app_slash));
    parsed.stream = std::string(path.substr(app_slash + 1));
    if (query != std::string_view::npos) parsed.stream += std::string(path_and_query.substr(query));
    parsed.tc_url = "rtmp://" + std::string(authority) + "/" + parsed.application;
    return parsed;
}

RtmpSourceClient::RtmpSourceClient(std::string source_url)
    : RtmpSourceClient(std::move(source_url), Options{}) {}

RtmpSourceClient::RtmpSourceClient(std::string source_url, Options options)
    : source_url_(std::move(source_url)), options_(options), encoder_(options_.chunk_size) {}

RtmpSourceClient::~RtmpSourceClient() {
    if (fd_ >= 0) ::close(fd_);
}

core::Result<int> RtmpSourceClient::connect_socket(
    const RtmpSourceUrl& parsed, const ContinuePredicate& should_continue) const {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(parsed.port);
    const int gai = ::getaddrinfo(parsed.host.c_str(), service.c_str(), &hints, &addresses);
    if (gai != 0) return network_error(core::ErrorCode::NotFound, "RTMP DNS lookup failed: " + std::string(gai_strerror(gai)));

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
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0) {
                ::freeaddrinfo(addresses);
                return fd;
            }
            last_error = std::strerror(socket_error == 0 ? errno : socket_error);
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    if (!should_continue()) return network_error(core::ErrorCode::OperationCanceled, "RTMP source stopped");
    if (std::chrono::steady_clock::now() >= deadline) {
        return network_error(core::ErrorCode::ConnectionTimedOut, "RTMP TCP connection timed out");
    }
    return network_error(core::ErrorCode::ConnectionClosed, "RTMP TCP connection failed: " + last_error);
}

void RtmpSourceClient::fail(core::ErrorCode code, core::ErrorCategory category, std::string message) {
    if (error_) return;
    error_.emplace(code, category, std::move(message));
    state_ = State::Failed;
}

void RtmpSourceClient::queue(std::span<const std::byte> bytes) {
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void RtmpSourceClient::queue_message(const RtmpMessage& message) {
    std::vector<std::byte> encoded;
    encoder_.encode_message(message, encoded);
    queue(encoded);
}

bool RtmpSourceClient::drain_output() {
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
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
             "RTMP send failed: " + std::string(std::strerror(errno)));
        return false;
    }
    output_.clear();
    output_offset_ = 0;
    handshake_input_.clear();
    return true;
}

void RtmpSourceClient::send_connect(const RtmpSourceUrl& parsed) {
    queue_message(command(0, {Amf0Value::string("connect"), Amf0Value::number(transaction_id_++),
                              Amf0Value::object({
                                  {"app", Amf0Value::string(parsed.application)},
                                  {"type", Amf0Value::string("nonprivate")},
                                  {"flashVer", Amf0Value::string("LNX 9,0,124,2")},
                                  {"tcUrl", Amf0Value::string(parsed.tc_url)},
                                  {"fpad", Amf0Value::boolean(false)},
                                  {"capabilities", Amf0Value::number(15)},
                                  {"audioCodecs", Amf0Value::number(4071)},
                                  {"videoCodecs", Amf0Value::number(252)},
                                  {"videoFunction", Amf0Value::number(1)},
                                  {"objectEncoding", Amf0Value::number(0)},
                              })}));
    state_ = State::Connecting;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpSourceClient::send_create_stream() {
    queue_message(command(0, {Amf0Value::string("createStream"), Amf0Value::number(transaction_id_++),
                              Amf0Value::null()}));
    state_ = State::CreatingStream;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpSourceClient::send_play(const RtmpSourceUrl& parsed) {
    // Tell the origin this is a low-latency live playback buffer.
    RtmpMessage buffer;
    buffer.chunk_stream_id = 2;
    buffer.message_stream_id = 0;
    buffer.message_type_id = static_cast<std::uint8_t>(MessageTypeId::UserControlMessage);
    append_be16(buffer.payload, 3); // SetBufferLength
    append_be32(buffer.payload, message_stream_id_);
    append_be32(buffer.payload, 1000);
    queue_message(buffer);

    queue_message(command(message_stream_id_,
                          {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                           Amf0Value::string(parsed.stream), Amf0Value::number(-2),
                           Amf0Value::number(-1), Amf0Value::boolean(true)}));
    state_ = State::Starting;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;
}

void RtmpSourceClient::handle_command(const RtmpMessage& message) {
    const auto payload = command_payload(message);
    auto decoded = protocol::amf0::decode_all(payload);
    if (!decoded || decoded.value().empty() || !decoded.value()[0].is_string()) return;
    const auto& values = decoded.value();
    const std::string& name = values[0].as_string();
    if (name == "_error") {
        fail(core::ErrorCode::Unauthorized, core::ErrorCategory::Authentication,
             "RTMP origin rejected a command");
        return;
    }
    if (state_ == State::Connecting && name == "_result") {
        send_create_stream();
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
    if (code == "NetStream.Play.Start" || code == "NetStream.Play.Reset") {
        if (state_ != State::Streaming) {
            state_ = State::Streaming;
            last_media_at_ = std::chrono::steady_clock::now();
            if (playing_handler_) playing_handler_();
        }
    } else if (code.find("Failed") != std::string::npos ||
               code.find("StreamNotFound") != std::string::npos ||
               code.find("Rejected") != std::string::npos ||
               code == "NetStream.Play.Stop" || code == "NetStream.Play.UnpublishNotify") {
        fail(core::ErrorCode::NotFound, core::ErrorCategory::Network,
             "RTMP playback failed: " + (code.empty() ? std::string("unknown status") : code));
    }
}

void RtmpSourceClient::handle_user_control(const RtmpMessage& message) {
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

void RtmpSourceClient::handle_media(RtmpMessage message) {
    if (state_ != State::Streaming) {
        state_ = State::Streaming; // Some origins pipeline media before onStatus.
        if (playing_handler_) playing_handler_();
    }
    last_media_at_ = std::chrono::steady_clock::now();
    if (media_handler_) {
        auto result = media_handler_(message);
        if (!result) fail(result.error().code(), result.error().category(), result.error().message());
    }
}

void RtmpSourceClient::handle_aggregate(const RtmpMessage& aggregate) {
    const auto bytes = std::span<const std::byte>(aggregate.payload);
    std::size_t offset = 0;
    std::optional<std::uint32_t> first_timestamp;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 15) {
            fail(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                 "truncated RTMP aggregate tag");
            return;
        }
        const auto type = static_cast<std::uint8_t>(bytes[offset]);
        const std::uint32_t data_size = read_be24(bytes, offset + 1);
        const std::uint32_t tag_timestamp =
            read_be24(bytes, offset + 4) | (static_cast<std::uint32_t>(bytes[offset + 7]) << 24);
        const std::size_t payload_at = offset + 11;
        const std::size_t previous_tag_at = payload_at + data_size;
        if (previous_tag_at + 4 > bytes.size()) {
            fail(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                 "RTMP aggregate tag payload exceeds message bounds");
            return;
        }
        if (read_be32(bytes, previous_tag_at) != data_size + 11) {
            fail(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                 "RTMP aggregate previous-tag size mismatch");
            return;
        }
        if (!first_timestamp) first_timestamp = tag_timestamp;
        if (type == static_cast<std::uint8_t>(MessageTypeId::Audio) ||
            type == static_cast<std::uint8_t>(MessageTypeId::Video)) {
            RtmpMessage media;
            media.chunk_stream_id = aggregate.chunk_stream_id;
            media.message_stream_id = aggregate.message_stream_id;
            media.message_type_id = type;
            media.timestamp = aggregate.timestamp + (tag_timestamp - *first_timestamp);
            media.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_at),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(previous_tag_at));
            handle_media(std::move(media));
            if (error_) return;
        }
        offset = previous_tag_at + 4;
    }
}

void RtmpSourceClient::handle_message(RtmpMessage message) {
    const auto type = static_cast<MessageTypeId>(message.message_type_id);
    if (type == MessageTypeId::Amf0Command || type == MessageTypeId::Amf3Command) {
        handle_command(message);
    } else if (type == MessageTypeId::UserControlMessage) {
        handle_user_control(message);
    } else if (type == MessageTypeId::Audio || type == MessageTypeId::Video) {
        handle_media(std::move(message));
    } else if (type == MessageTypeId::Aggregate) {
        handle_aggregate(message);
    }
}

core::Result<void> RtmpSourceClient::run(const ContinuePredicate& should_continue,
                                         MediaHandler media_handler,
                                         PlayingHandler playing_handler) {
    auto parsed = parse_rtmp_source_url(source_url_);
    if (!parsed) return parsed.error();
    auto connected = connect_socket(parsed.value(), should_continue);
    if (!connected) return connected.error();
    fd_ = connected.value();
    media_handler_ = std::move(media_handler);
    playing_handler_ = std::move(playing_handler);
    error_.reset();
    output_.clear();
    output_offset_ = 0;
    message_stream_id_ = 0;
    transaction_id_ = 1.0;

    decoder_.emplace(options_.max_message_size);
    decoder_->set_message_handler([this](RtmpMessage message) { handle_message(std::move(message)); });
    decoder_->set_error_handler([this](core::Error error) {
        fail(error.code(), error.category(), error.message());
    });

    std::vector<std::byte> c0c1;
    c0c1.reserve(1 + protocol::handshake::kHandshakeChunkSize);
    c0c1.push_back(static_cast<std::byte>(protocol::handshake::kRtmpVersion));
    c0c1.resize(1 + protocol::handshake::kHandshakeChunkSize, std::byte{0});
    for (std::size_t i = 9; i < c0c1.size(); ++i) {
        c0c1[i] = static_cast<std::byte>((i * 31u + 7u) & 0xffu);
    }
    queue(c0c1);
    state_ = State::Handshaking;
    state_deadline_ = std::chrono::steady_clock::now() + options_.command_timeout;

    std::array<std::byte, 64 * 1024> input{};
    while (should_continue() && !error_) {
        const auto now = std::chrono::steady_clock::now();
        if (state_ != State::Streaming && now >= state_deadline_) {
            fail(core::ErrorCode::ConnectionTimedOut, core::ErrorCategory::Network,
                 "RTMP handshake/command timed out");
            break;
        }
        if (state_ == State::Streaming && now - last_media_at_ >= options_.media_timeout) {
            fail(core::ErrorCode::ConnectionTimedOut, core::ErrorCategory::Network,
                 "RTMP source stalled: no audio or video for 45 seconds");
            break;
        }

        short events = POLLIN;
        if (output_offset_ < output_.size()) events = static_cast<short>(events | POLLOUT);
        pollfd item{fd_, events, 0};
        const int polled = ::poll(&item, 1, 250);
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) {
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP poll failed: " + std::string(std::strerror(errno)));
            break;
        }
        if ((item.revents & POLLOUT) != 0 && !drain_output()) break;
        if ((item.revents & (POLLERR | POLLNVAL)) != 0) {
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP socket reported a transport error");
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
                        if (handshake_input_[0] != static_cast<std::byte>(protocol::handshake::kRtmpVersion)) {
                            fail(core::ErrorCode::MalformedHandshake, core::ErrorCategory::Protocol,
                                 "RTMP origin returned an unsupported handshake version");
                            break;
                        }
                        // C2 is an exact echo of S1.
                        queue(std::span<const std::byte>(handshake_input_).subspan(
                            1, protocol::handshake::kHandshakeChunkSize));
                        std::vector<std::byte> set_chunk_size;
                        encoder_.encode_set_chunk_size(options_.chunk_size, set_chunk_size);
                        queue(set_chunk_size);
                        send_connect(parsed.value());
                        const auto trailing = std::span<const std::byte>(handshake_input_).subspan(
                            kHandshakeResponseSize);
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
                    queue(ack);
                    decoder_->mark_acknowledged();
                }
                if (state_ == State::CreatingStream && message_stream_id_ != 0) {
                    send_play(parsed.value());
                }
                if (error_) break;
                continue;
            }
            if (received == 0) {
                fail(core::ErrorCode::ConnectionClosed, core::ErrorCategory::Network,
                     "RTMP source connection closed");
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fail(core::ErrorCode::ConnectionReset, core::ErrorCategory::Network,
                 "RTMP receive failed: " + std::string(std::strerror(errno)));
            break;
        }
    }

    ::close(fd_);
    fd_ = -1;
    if (error_) return *error_;
    return {};
}

} // namespace rtmp_server::transcoding::native
