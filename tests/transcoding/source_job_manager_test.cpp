#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "rtmp_server/transcoding/native/source_job_manager.hpp"

using namespace rtmp_server;
using namespace rtmp_server::transcoding::native;

namespace {

// Source Transcode now spawns real ffmpeg subprocesses (see
// src/transcoding/native/source_job_manager.cpp) instead of running the
// FFmpeg-free native decode/encode pipeline. build_argv() probes encoder
// availability by running `<ffmpeg> -hide_banner -loglevel error -h
// encoder=<name>` (BackendRegistry::encoder_is_available), so a real ffmpeg
// binary isn't required in the test sandbox — a stub that exits 0 for that
// probe (same technique tests/transcoding/supervisor_test.cpp uses for
// TranscoderSupervisor) is enough to exercise the manager's lifecycle. It
// never actually reaches a spawned "encode": posix_spawn'ing the same stub as
// the job's ffmpeg process just runs an infinite sleep loop until SIGTERM.
class FakeEncoder {
public:
    FakeEncoder() {
        path_ = std::filesystem::temp_directory_path() /
                ("streamforge-fake-source-encoder-" + std::to_string(::getpid()));
        std::ofstream script(path_);
        script << "#!/bin/sh\n"
                  "if [ \"$1\" = \"-hide_banner\" ] && [ \"$2\" = \"-loglevel\" ] && [ \"$3\" = \"error\" ]; then exit 0; fi\n"
                  "trap 'exit 0' TERM INT\n"
                  "while :; do sleep 1; done\n";
        script.close();
        std::filesystem::permissions(
            path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace);
    }
    ~FakeEncoder() { std::filesystem::remove(path_); }
    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

SourceJobManagerOptions test_options(const FakeEncoder& encoder) {
    SourceJobManagerOptions options;
    options.ffmpeg_path = encoder.string();
    options.rtmp_port = 19350;
    options.max_restart_attempts = 1;
    options.stop_timeout = std::chrono::seconds(1);
    return options;
}

} // namespace

TEST(SourceJobManagerTest, CreatePublishesMasterPlaylistWithEveryRendition) {
    std::vector<hls::Rendition> published;
    std::size_t publish_count = 0;
    std::string published_application;
    std::string published_master;

    SourceJobManager::Hooks hooks;
    hooks.set_renditions = [&](const std::string& application, const std::string& master,
                               std::vector<hls::Rendition> renditions) {
        published_application = application;
        published_master = master;
        published = std::move(renditions);
        ++publish_count;
    };
    FakeEncoder encoder;
    SourceJobManager manager(std::move(hooks), test_options(encoder));

    SourceJobConfig config;
    config.application = "live";
    config.name = "demo";
    config.source_url = "rtmp://127.0.0.1:1/unreachable/source";
    config.template_name = "test";
    config.auto_restart = false;
    config.renditions.push_back(RenditionSpec{"480p", "demo_480p", 854, 480, 500'000, 60, 96'000});
    config.renditions.push_back(RenditionSpec{"720p", "demo_720p", 1280, 720, 2'500'000, 60, 128'000});

    auto snapshot = manager.create(config);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().application, "live");
    EXPECT_EQ(snapshot.value().name, "demo");
    EXPECT_EQ(snapshot.value().master_hls_path, "/hls/live/demo/master.m3u8");
    EXPECT_TRUE(snapshot.value().enabled);
    EXPECT_EQ(snapshot.value().renditions.size(), 2u);

    EXPECT_GE(publish_count, 1u);
    EXPECT_EQ(published_application, "live");
    EXPECT_EQ(published_master, "demo");
    ASSERT_EQ(published.size(), 2u);
    EXPECT_EQ(published[0].uri, "../demo_480p/index.m3u8");
    EXPECT_EQ(published[1].uri, "../demo_720p/index.m3u8");

    manager.stop_all();
}

TEST(SourceJobManagerTest, LifecycleTransitionsSucceed) {
    SourceJobManager::Hooks hooks;
    hooks.set_renditions = [](const std::string&, const std::string&, std::vector<hls::Rendition>) {};
    FakeEncoder encoder;
    SourceJobManager manager(std::move(hooks), test_options(encoder));

    SourceJobConfig config;
    config.application = "live";
    config.name = "demo";
    config.source_url = "rtmp://127.0.0.1:1/unreachable/source";
    config.template_name = "test";
    config.auto_restart = false;
    config.renditions.push_back(RenditionSpec{"480p", "demo_480p", 854, 480, 500'000, 60, 96'000});

    ASSERT_TRUE(manager.create(config).ok());

    auto disabled = manager.set_enabled("live", "demo", false);
    ASSERT_TRUE(disabled.ok());
    EXPECT_EQ(disabled.value().status, "disabled");
    EXPECT_FALSE(disabled.value().enabled);

    auto reenabled = manager.set_enabled("live", "demo", true);
    ASSERT_TRUE(reenabled.ok());
    EXPECT_TRUE(reenabled.value().enabled);

    auto restarted = manager.restart("live", "demo");
    EXPECT_TRUE(restarted.ok());

    EXPECT_TRUE(manager.remove("live", "demo"));
    EXPECT_FALSE(manager.remove("live", "demo")) << "second remove of the same job must fail";

    auto missing = manager.restart("live", "demo");
    EXPECT_FALSE(missing.ok());
}

TEST(SourceJobManagerTest, CreateRejectsUnsupportedSourceUrlScheme) {
    SourceJobManager::Hooks hooks;
    FakeEncoder encoder;
    SourceJobManager manager(std::move(hooks), test_options(encoder));

    SourceJobConfig config;
    config.application = "live";
    config.name = "demo";
    config.source_url = "ftp://example.com/source";
    config.renditions.push_back(RenditionSpec{"480p", "demo_480p", 854, 480, 500'000, 60, 96'000});

    auto result = manager.create(config);
    EXPECT_FALSE(result.ok());
}
