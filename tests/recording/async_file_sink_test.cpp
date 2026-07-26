#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/media/flv/flv_writer.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/recording/async_file_sink.hpp"
#include "rtmp_server/recording/recorder.hpp"
#include "rtmp_server/recording/retention.hpp"

namespace fs = std::filesystem;
using namespace rtmp_server;
using recording::AsyncFileSink;
using recording::Recorder;

namespace {

constexpr auto kWait = std::chrono::seconds(10);

// Unique scratch directory per test, removed on destruction (RAII).
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rtmp_phase6_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::vector<std::byte> bytes_of(std::size_t n, std::byte fill = std::byte{0xAB}) {
    return std::vector<std::byte>(n, fill);
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> out;
    char c = 0;
    while (in.get(c)) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

protocol::chunk::RtmpMessage media_message(std::uint8_t type, std::uint32_t timestamp, std::size_t size) {
    protocol::chunk::RtmpMessage message;
    message.message_type_id = type;
    message.timestamp = timestamp;
    message.payload = bytes_of(size);
    return message;
}

} // namespace

// --- Basic lifecycle ------------------------------------------------------

TEST(AsyncFileSinkTest, WritesThroughTemporaryFileAndAtomicallyRenames) {
    TempDir dir;
    const std::string target = dir.file("recording.flv");

    auto opened = AsyncFileSink::open(target);
    ASSERT_TRUE(opened.ok()) << opened.error().message();
    auto sink = std::move(opened).value();

    // While recording, only the .part file exists — a reader watching the
    // output directory never sees a partial "finished" recording.
    ASSERT_TRUE(sink->append(bytes_of(1024)).ok());
    EXPECT_TRUE(fs::exists(target + ".part"));
    EXPECT_FALSE(fs::exists(target));

    ASSERT_TRUE(sink->finalize().ok());
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    EXPECT_TRUE(fs::exists(target));
    EXPECT_FALSE(fs::exists(target + ".part"));
    EXPECT_EQ(fs::file_size(target), 1024u);

    const auto stats = sink->stats();
    EXPECT_TRUE(stats.finalized);
    EXPECT_TRUE(stats.renamed);
    EXPECT_EQ(stats.bytes_written, 1024u);
}

TEST(AsyncFileSinkTest, PatchRewritesBytesInPlaceAfterQueuedAppends) {
    TempDir dir;
    const std::string target = dir.file("patch.bin");
    auto sink = std::move(AsyncFileSink::open(target).value());

    ASSERT_TRUE(sink->append(bytes_of(16, std::byte{0x00})).ok());
    // Patch is queued behind the append and must not overtake it.
    const std::vector<std::byte> patch(4, std::byte{0xFF});
    ASSERT_TRUE(sink->patch(4, patch).ok());

    ASSERT_TRUE(sink->finalize().ok());
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    const auto data = read_file(target);
    ASSERT_EQ(data.size(), 16u);
    EXPECT_EQ(data[3], std::byte{0x00});
    EXPECT_EQ(data[4], std::byte{0xFF});
    EXPECT_EQ(data[7], std::byte{0xFF});
    EXPECT_EQ(data[8], std::byte{0x00});
    EXPECT_EQ(sink->stats().patches_applied, 1u);
}

TEST(AsyncFileSinkTest, OpenFailsForUnwritableDirectory) {
    auto opened = AsyncFileSink::open("/nonexistent-directory-phase6/out.flv");
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().category(), core::ErrorCategory::Storage);
    EXPECT_EQ(opened.error().code(), core::ErrorCode::StorageUnavailable);
}

// --- Bounded queue / overflow policy --------------------------------------

TEST(AsyncFileSinkTest, QueueOverflowIsRejectedExplicitlyAndCounted) {
    TempDir dir;
    AsyncFileSink::Options options;
    options.max_queue_bytes = 4096;
    auto sink = std::move(AsyncFileSink::open(dir.file("bounded.bin"), options).value());

    // Push far more than the queue can hold. Some appends succeed (the
    // writer drains concurrently) but any rejection must be an explicit
    // ResourceExhausted error, never a silent drop.
    int rejections = 0;
    for (int i = 0; i < 200; ++i) {
        auto result = sink->append(bytes_of(2048));
        if (!result.ok()) {
            EXPECT_EQ(result.error().code(), core::ErrorCode::ResourceExhausted);
            EXPECT_EQ(result.error().category(), core::ErrorCategory::Storage);
            ++rejections;
        }
    }

    // The queue is bounded at all times.
    EXPECT_LE(sink->pending_bytes(), options.max_queue_bytes);
    if (rejections > 0) EXPECT_GT(sink->stats().overflow_events, 0u);

    ASSERT_TRUE(sink->finalize().ok());
    ASSERT_TRUE(sink->wait_for_completion(kWait));
}

TEST(RecorderWithAsyncSinkTest, QueueOverflowDropsFramesButKeepsRecording) {
    TempDir dir;
    AsyncFileSink::Options sink_options;
    sink_options.max_queue_bytes = 8 * 1024;
    auto sink = std::move(AsyncFileSink::open(dir.file("overflow.flv"), sink_options).value());

    recording::RecorderConfig config;
    config.max_queued_bytes = 1024 * 1024; // let the sink's own bound bite first
    Recorder recorder(*sink, config);

    for (int i = 0; i < 400; ++i) {
        recorder.on_video(media_message(9, static_cast<std::uint32_t>(i * 33), 4096));
    }
    recorder.finalize();
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    // Overflow is backpressure, not corruption: recording continued and the
    // file was still finalized correctly.
    EXPECT_FALSE(recorder.stats().failed);
    EXPECT_GT(recorder.stats().tags_written, 0u);
    EXPECT_TRUE(sink->stats().renamed);
    // Whatever was written must still be a structurally valid FLV.
    const auto data = read_file(dir.file("overflow.flv"));
    auto parsed = media::flv::parse_flv(data);
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message());
}

// --- Failure policy -------------------------------------------------------

TEST(AsyncFileSinkTest, DiskSpaceExhaustionMarksSinkUnhealthyAndSkipsPublish) {
    TempDir dir;
    AsyncFileSink::Options options;
    // No real filesystem has this much free space, so the monitor must trip.
    options.min_free_bytes = ~0ULL / 2;
    options.disk_check_interval_bytes = 1; // check after the first write
    auto sink = std::move(AsyncFileSink::open(dir.file("full.flv"), options).value());

    ASSERT_TRUE(sink->append(bytes_of(4096)).ok());
    // Give the writer thread a moment to perform the statvfs check.
    for (int i = 0; i < 200 && sink->healthy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(sink->healthy());
    EXPECT_EQ(sink->failure().code(), core::ErrorCode::StorageUnavailable);

    ASSERT_TRUE(sink->finalize().ok());
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    // A failed recording is deliberately NOT published under the final name;
    // the .part file is left as evidence.
    EXPECT_FALSE(sink->stats().renamed);
    EXPECT_FALSE(fs::exists(dir.file("full.flv")));
    EXPECT_TRUE(fs::exists(dir.file("full.flv") + ".part"));
}

TEST(AsyncFileSinkTest, AppendAfterFailureReturnsTheRecordedError) {
    TempDir dir;
    AsyncFileSink::Options options;
    options.min_free_bytes = ~0ULL / 2;
    options.disk_check_interval_bytes = 1;
    auto sink = std::move(AsyncFileSink::open(dir.file("failed.bin"), options).value());

    ASSERT_TRUE(sink->append(bytes_of(64)).ok());
    for (int i = 0; i < 200 && sink->healthy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(sink->healthy());

    auto result = sink->append(bytes_of(64));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::StorageUnavailable);
}

TEST(AsyncFileSinkTest, DestructorFinalizesWithoutAnExplicitFinalizeCall) {
    TempDir dir;
    const std::string target = dir.file("raii.bin");
    {
        auto sink = std::move(AsyncFileSink::open(target).value());
        ASSERT_TRUE(sink->append(bytes_of(256)).ok());
        // No finalize() — the destructor must still stop the writer thread
        // and release the fd rather than leaking or hanging.
    }
    EXPECT_TRUE(fs::exists(target));
    EXPECT_EQ(fs::file_size(target), 256u);
}

// --- Publisher disconnect mid-recording -----------------------------------

TEST(RecorderWithAsyncSinkTest, AbruptPublisherDisconnectStillProducesValidFlv) {
    TempDir dir;
    const std::string target = dir.file("disconnect.flv");
    auto sink = std::move(AsyncFileSink::open(target).value());
    Recorder recorder(*sink);

    recorder.on_video(media_message(9, 0, 512));
    recorder.on_audio(media_message(8, 20, 128));
    recorder.on_video(media_message(9, 33, 512));
    // Simulates CommandSession's disconnect path firing mid-stream.
    recorder.finalize();
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    EXPECT_FALSE(recorder.stats().failed);
    const auto data = read_file(target);
    auto parsed = media::flv::parse_flv(data);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    // onMetaData + 3 media tags.
    EXPECT_EQ(parsed.value().tags.size(), 4u);
    EXPECT_EQ(parsed.value().previous_tag_size0, 0u);
}

TEST(RecorderWithAsyncSinkTest, FinalizeIsIdempotent) {
    TempDir dir;
    auto sink = std::move(AsyncFileSink::open(dir.file("idem.flv")).value());
    Recorder recorder(*sink);
    recorder.on_video(media_message(9, 0, 64));
    recorder.finalize();
    recorder.finalize();
    recorder.finalize();
    EXPECT_TRUE(sink->wait_for_completion(kWait));
    EXPECT_TRUE(sink->stats().renamed);
}

// --- The central Phase 6 invariant ----------------------------------------

TEST(NoDiskIoOnMediaThreadTest, EveryWriteHappensOnTheSinkWriterThreadNotTheCaller) {
    TempDir dir;
    auto sink = std::move(AsyncFileSink::open(dir.file("thread.flv")).value());
    Recorder recorder(*sink);

    const std::thread::id media_thread = std::this_thread::get_id();
    // The thread that owns every pwrite/fsync/rename is, by construction, a
    // different thread from the one driving the recorder.
    ASSERT_NE(sink->writer_thread_id(), media_thread);
    ASSERT_NE(sink->writer_thread_id(), std::thread::id{});

    for (int i = 0; i < 500; ++i) {
        recorder.on_video(media_message(9, static_cast<std::uint32_t>(i * 33), 8192));
    }
    recorder.finalize();
    ASSERT_TRUE(sink->wait_for_completion(kWait));

    // Bytes really did reach the disk, on that other thread.
    EXPECT_GT(sink->stats().bytes_written, 0u);
    EXPECT_TRUE(sink->stats().renamed);
    EXPECT_NE(sink->writer_thread_id(), media_thread);
}

TEST(NoDiskIoOnMediaThreadTest, CommandPathCallsReturnPromptlyWhileTheDiskIsBusy) {
    TempDir dir;
    auto sink = std::move(AsyncFileSink::open(dir.file("latency.flv")).value());
    Recorder recorder(*sink);

    // Drive ~8 MB of media through the recorder and measure the WORST
    // single call latency on the simulated media thread. If any disk write
    // were performed synchronously here, a single call would block for the
    // duration of that write (and fsync at finalize would block for far
    // longer). The bound is deliberately generous so the test is about the
    // architecture, not about disk speed.
    std::chrono::nanoseconds worst{0};
    for (int i = 0; i < 1000; ++i) {
        auto message = media_message(9, static_cast<std::uint32_t>(i * 33), 8192);
        const auto start = std::chrono::steady_clock::now();
        recorder.on_video(message);
        worst = std::max(worst, std::chrono::steady_clock::now() - start);
    }

    const auto finalize_start = std::chrono::steady_clock::now();
    recorder.finalize(); // includes the fsync+rename request
    const auto finalize_latency = std::chrono::steady_clock::now() - finalize_start;

    // finalize() must not wait for fsync: it only signals the writer.
    EXPECT_LT(finalize_latency, std::chrono::seconds(1));
    EXPECT_LT(worst, std::chrono::seconds(1));

    // The actual durable work completes afterwards, off the media thread.
    ASSERT_TRUE(sink->wait_for_completion(kWait));
    EXPECT_TRUE(sink->stats().renamed);
}

// --- Process-restart behaviour --------------------------------------------

TEST(ProcessRestartTest, LeftoverPartFileFromACrashIsNotMistakenForARecording) {
    TempDir dir;
    const std::string target = dir.file("crashed.flv");

    // Simulate a crash: a .part file exists with no rename having happened.
    {
        std::ofstream out(target + ".part", std::ios::binary);
        out << "partial";
    }
    ASSERT_TRUE(fs::exists(target + ".part"));
    ASSERT_FALSE(fs::exists(target));

    // Retention only ever considers finished ".flv" recordings, so the
    // crash artifact is invisible to it and is never served as a recording.
    recording::RetentionPolicy policy;
    policy.max_files = 10;
    auto plan = recording::apply_retention(dir.path().string(), policy, ".flv");
    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().files_kept, 0u);
    EXPECT_TRUE(plan.value().to_delete.empty());

    // A restart reusing the same path truncates the stale .part rather than
    // appending to it, so the new recording is not corrupted by old bytes.
    {
        auto sink = std::move(AsyncFileSink::open(target).value());
        ASSERT_TRUE(sink->append(bytes_of(10, std::byte{0x01})).ok());
        ASSERT_TRUE(sink->finalize().ok());
        ASSERT_TRUE(sink->wait_for_completion(kWait));
    }
    ASSERT_TRUE(fs::exists(target));
    EXPECT_EQ(fs::file_size(target), 10u); // not 7 + 10
}
