#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"

namespace rtmp_server::transcoding::native {

// A parsed, plain-RTMP playback URL. Query parameters belong to the stream
// name (the convention used by token-authenticated RTMP origins), while tc_url
// identifies the application connection endpoint.
struct RtmpSourceUrl {
    std::string host;
    std::uint16_t port = 1935;
    std::string application;
    std::string stream;
    std::string tc_url;
};

[[nodiscard]] core::Result<RtmpSourceUrl> parse_rtmp_source_url(std::string_view url);

// Blocking, cancellation-aware RTMP playback client used only by a source-job
// worker thread. It owns the complete client side of RTMP: DNS/TCP, simple
// handshake, connect/createStream/play, chunk reassembly, acknowledgement,
// ping response, aggregate-message unpacking and bounded liveness timeouts.
// Media remains in its original FLV/RTMP tag form and is delivered to the
// native source transcoder by callback; no external process is involved.
class RtmpSourceClient {
public:
    using ContinuePredicate = std::function<bool()>;
    using MediaHandler =
        std::function<core::Result<void>(const protocol::chunk::RtmpMessage&)>;
    using PlayingHandler = std::function<void()>;

    struct Options {
        std::chrono::seconds connect_timeout{10};
        std::chrono::seconds command_timeout{15};
        std::chrono::seconds media_timeout{45};
        std::uint32_t chunk_size = 4096;
        std::uint32_t max_message_size = 16u * 1024u * 1024u;
    };

    explicit RtmpSourceClient(std::string source_url);
    RtmpSourceClient(std::string source_url, Options options);
    ~RtmpSourceClient();
    RtmpSourceClient(const RtmpSourceClient&) = delete;
    RtmpSourceClient& operator=(const RtmpSourceClient&) = delete;

    [[nodiscard]] core::Result<void> run(const ContinuePredicate& should_continue,
                                         MediaHandler media_handler,
                                         PlayingHandler playing_handler = {});

private:
    enum class State { Handshaking, Connecting, CreatingStream, Starting, Streaming, Failed };

    [[nodiscard]] core::Result<int> connect_socket(const RtmpSourceUrl& parsed,
                                                    const ContinuePredicate& should_continue) const;
    void fail(core::ErrorCode code, core::ErrorCategory category, std::string message);
    void queue(std::span<const std::byte> bytes);
    void queue_message(const protocol::chunk::RtmpMessage& message);
    [[nodiscard]] bool drain_output();
    void send_connect(const RtmpSourceUrl& parsed);
    void send_create_stream();
    void send_play(const RtmpSourceUrl& parsed);
    void handle_message(protocol::chunk::RtmpMessage message);
    void handle_command(const protocol::chunk::RtmpMessage& message);
    void handle_user_control(const protocol::chunk::RtmpMessage& message);
    void handle_media(protocol::chunk::RtmpMessage message);
    void handle_aggregate(const protocol::chunk::RtmpMessage& message);

    std::string source_url_;
    Options options_;
    int fd_ = -1;
    State state_ = State::Failed;
    protocol::chunk::ChunkEncoder encoder_;
    std::optional<protocol::chunk::ChunkDecoder> decoder_;
    std::vector<std::byte> handshake_input_;
    std::vector<std::byte> output_;
    std::size_t output_offset_ = 0;
    std::uint32_t message_stream_id_ = 0;
    double transaction_id_ = 1.0;
    std::optional<core::Error> error_;
    MediaHandler media_handler_;
    PlayingHandler playing_handler_;
    std::chrono::steady_clock::time_point state_deadline_{};
    std::chrono::steady_clock::time_point last_media_at_{};
};

} // namespace rtmp_server::transcoding::native
