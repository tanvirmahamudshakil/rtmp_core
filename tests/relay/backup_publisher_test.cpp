#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/relay/backup_publisher.hpp"

using namespace rtmp_server;
using rtmp_server::core::ErrorCode;
using rtmp_server::protocol::amf0::Amf0Value;
using rtmp_server::protocol::chunk::MessageTypeId;
using rtmp_server::protocol::chunk::RtmpMessage;
using rtmp_server::relay::BackupPublisherConfig;
using rtmp_server::relay::BackupPublisherManager;
using rtmp_server::relay::BackupPublisherState;

namespace {

bool send_all(int fd, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool receive_exact(int fd, std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = ::recv(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

RtmpMessage make_command(std::uint32_t stream_id, const std::vector<Amf0Value>& values) {
    RtmpMessage message;
    message.chunk_stream_id = 3;
    message.message_stream_id = stream_id;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& value : values) protocol::amf0::encode(value, message.payload);
    return message;
}

std::string command_name(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    auto values = protocol::amf0::decode_all(message.payload);
    if (!values || values.value().empty() || !values.value()[0].is_string()) return {};
    return values.value()[0].as_string();
}

// A minimal RTMP playback origin: answers connect/createStream/play and then
// streams one video tag. Standing in for a real backup encoder/origin.
class FakeBackupSource {
public:
    FakeBackupSource() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 1);
        socklen_t size = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }

    ~FakeBackupSource() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) ::close(listener_);
    }

    [[nodiscard]] std::string url() const {
        return "rtmp://127.0.0.1:" + std::to_string(port_) + "/live/backup";
    }

private:
    void serve() {
        const int peer = ::accept(listener_, nullptr, nullptr);
        if (peer < 0) return;

        std::array<std::byte, 1 + protocol::handshake::kHandshakeChunkSize> c0c1{};
        if (!receive_exact(peer, c0c1)) {
            ::close(peer);
            return;
        }
        std::vector<std::byte> handshake(1 + 2 * protocol::handshake::kHandshakeChunkSize,
                                         std::byte{0});
        handshake[0] = static_cast<std::byte>(protocol::handshake::kRtmpVersion);
        std::copy(c0c1.begin() + 1, c0c1.end(),
                  handshake.begin() + 1 + protocol::handshake::kHandshakeChunkSize);
        send_all(peer, handshake);
        std::array<std::byte, protocol::handshake::kHandshakeChunkSize> c2{};
        if (!receive_exact(peer, c2)) {
            ::close(peer);
            return;
        }

        protocol::chunk::ChunkEncoder encoder;
        protocol::chunk::ChunkDecoder decoder(1024 * 1024);
        bool media_sent = false;
        const auto send_message = [&](const RtmpMessage& message) {
            std::vector<std::byte> bytes;
            encoder.encode_message(message, bytes);
            send_all(peer, bytes);
        };
        decoder.set_message_handler([&](RtmpMessage message) {
            const auto name = command_name(message);
            if (name == "connect") {
                send_message(make_command(0, {Amf0Value::string("_result"), Amf0Value::number(1),
                                              Amf0Value::object(), Amf0Value::object()}));
            } else if (name == "createStream") {
                send_message(make_command(0, {Amf0Value::string("_result"), Amf0Value::number(2),
                                              Amf0Value::null(), Amf0Value::number(1)}));
            } else if (name == "play") {
                send_message(make_command(
                    1, {Amf0Value::string("onStatus"), Amf0Value::number(0), Amf0Value::null(),
                        Amf0Value::object({{"code", Amf0Value::string("NetStream.Play.Start")}})}));
                RtmpMessage video;
                video.chunk_stream_id = 6;
                video.message_stream_id = 1;
                video.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
                video.timestamp = 7;
                video.payload = {std::byte{0x17}, std::byte{0x01}, std::byte{0},
                                 std::byte{0},    std::byte{0},    std::byte{0x5A}};
                send_message(video);
                media_sent = true;
            }
        });

        std::array<std::byte, 4096> input{};
        while (running_.load() && !media_sent) {
            pollfd item{peer, POLLIN, 0};
            if (::poll(&item, 1, 50) <= 0) continue;
            const auto received = ::recv(peer, input.data(), input.size(), 0);
            if (received > 0) {
                decoder.on_bytes_received(
                    std::span<const std::byte>(input.data(), static_cast<std::size_t>(received)));
                continue;
            }
            if (received < 0 && errno == EINTR) continue;
            break;
        }
        while (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::close(peer);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::jthread thread_;
};

// A fake packaging sink that just counts what it receives -- standing in for
// the real recorder_factory sink (HLS/DASH/transcode fan-out) the composition
// root would hand out.
class CountingSink final : public protocol::commands::RecorderSink {
public:
    void on_metadata(const protocol::chunk::RtmpMessage&) override {}
    void on_audio(const protocol::chunk::RtmpMessage&) override {}
    void on_video(const protocol::chunk::RtmpMessage& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++video_count_;
        last_marker_ = message.payload.empty() ? std::byte{0} : message.payload.back();
    }
    void finalize() override {
        std::lock_guard<std::mutex> lock(mutex_);
        finalized_ = true;
    }
    [[nodiscard]] int video_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return video_count_;
    }
    [[nodiscard]] bool finalized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return finalized_;
    }

private:
    mutable std::mutex mutex_;
    int video_count_ = 0;
    std::byte last_marker_{0};
    bool finalized_ = false;
};

class FakeStore final : public persistence::Store {
public:
    core::Result<void> upsert_application(const persistence::ApplicationRow&) override { return {}; }
    core::Result<void> delete_application(std::string_view) override { return {}; }
    core::Result<std::vector<persistence::ApplicationRow>> load_applications() override {
        return std::vector<persistence::ApplicationRow>{};
    }
    core::Result<void> upsert_stream(const persistence::StreamRow&) override { return {}; }
    core::Result<void> delete_stream(std::string_view, std::string_view) override { return {}; }
    core::Result<std::vector<persistence::StreamRow>> load_streams() override {
        return std::vector<persistence::StreamRow>{};
    }

    core::Result<void> upsert_backup_publisher(const persistence::BackupPublisherRow& row) override {
        rows[row.application + "/" + row.stream] = row;
        return {};
    }
    core::Result<void> delete_backup_publisher(std::string_view application,
                                               std::string_view stream) override {
        rows.erase(std::string(application) + "/" + std::string(stream));
        return {};
    }
    core::Result<std::vector<persistence::BackupPublisherRow>> load_backup_publishers() override {
        std::vector<persistence::BackupPublisherRow> out;
        for (const auto& [key, row] : rows) out.push_back(row);
        return out;
    }

    std::unordered_map<std::string, persistence::BackupPublisherRow> rows;
};

BackupPublisherConfig config(std::string backup_url, std::uint32_t failover_after = 1) {
    BackupPublisherConfig config;
    config.application = "live";
    config.stream = "main";
    config.backup_url = std::move(backup_url);
    config.failover_after_seconds = failover_after;
    config.restart_delay_seconds = 1;
    return config;
}

TEST(BackupPublisherManagerTest, RejectsAConfigThatIsNotAPlayableRtmpUrl) {
    FakeStore store;
    BackupPublisherManager manager(
        &store, [](std::string_view, std::string_view) { return true; },
        [](std::string_view, std::string_view) -> std::shared_ptr<protocol::commands::RecorderSink> {
            return nullptr;
        });
    EXPECT_FALSE(manager.upsert(config("https://example.com/live/backup")));
    EXPECT_FALSE(manager.upsert(config("rtmp://example.com/live")));
    auto missing_stream = config("rtmp://example.com/live/backup");
    missing_stream.stream.clear();
    EXPECT_FALSE(manager.upsert(missing_stream));
    EXPECT_TRUE(store.rows.empty());
}

TEST(BackupPublisherManagerTest, StoresAConfigAndReportsStandbyWhilePrimaryIsLive) {
    FakeStore store;
    BackupPublisherManager manager(
        &store, [](std::string_view, std::string_view) { return true; }, // primary always live
        [](std::string_view, std::string_view) -> std::shared_ptr<protocol::commands::RecorderSink> {
            return std::make_shared<CountingSink>();
        });

    auto created = manager.upsert(config("rtmp://backup.example.com/live/key"));
    ASSERT_TRUE(created) << created.error().message();
    EXPECT_EQ(created.value().url_redacted, "rtmp://backup.example.com/live/****");
    EXPECT_EQ(store.rows.size(), 1u);

    // Give the monitor a couple of ticks; the primary is always reported live,
    // so a worker must never even be created.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto items = manager.list("live");
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items.front().state, BackupPublisherState::Standby);
}

TEST(BackupPublisherManagerTest, RemovesAConfig) {
    FakeStore store;
    BackupPublisherManager manager(
        &store, [](std::string_view, std::string_view) { return true; },
        [](std::string_view, std::string_view) -> std::shared_ptr<protocol::commands::RecorderSink> {
            return nullptr;
        });
    ASSERT_TRUE(manager.upsert(config("rtmp://backup.example.com/live/key")));
    ASSERT_TRUE(manager.remove("live", "main"));
    EXPECT_TRUE(manager.list("live").empty());
    EXPECT_TRUE(store.rows.empty());

    auto missing = manager.remove("live", "main");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

// End-to-end: the primary is reported absent from the start, so the backup
// must be dialed, its media forwarded into the packaging sink, and the sink
// finalized again once the primary is reported back.
TEST(BackupPublisherManagerTest, ActivatesTheBackupWhenThePrimaryIsAbsentAndStandsDownWhenItReturns) {
    FakeBackupSource source;
    auto sink = std::make_shared<CountingSink>();
    std::atomic<bool> primary_live{false};

    BackupPublisherManager manager(
        nullptr, [&](std::string_view, std::string_view) { return primary_live.load(); },
        [&](std::string_view, std::string_view) -> std::shared_ptr<protocol::commands::RecorderSink> {
            return sink;
        });

    auto created = manager.upsert(config(source.url(), /*failover_after=*/0));
    ASSERT_TRUE(created) << created.error().message();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool activated = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink->video_count() > 0) {
            activated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(activated) << "backup never delivered media into the packaging sink";

    const auto active = manager.list("live");
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active.front().state, BackupPublisherState::Active);
    EXPECT_GT(active.front().activations, 0u);

    // The primary returns; the backup must stand down and finalize the sink it
    // was feeding, exactly as a real publisher's disconnect would.
    primary_live.store(true);
    const auto standby_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool stood_down = false;
    while (std::chrono::steady_clock::now() < standby_deadline) {
        if (sink->finalized()) {
            stood_down = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(stood_down) << "backup never released the sink once the primary returned";
}

} // namespace
