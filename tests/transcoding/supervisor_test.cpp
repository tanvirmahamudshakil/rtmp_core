#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

#include "rtmp_server/transcoding/supervisor.hpp"

namespace {

using namespace std::chrono_literals;

class FakeEncoder {
public:
    FakeEncoder() {
        path_ = std::filesystem::temp_directory_path() /
                ("streamforge-fake-encoder-" + std::to_string(::getpid()));
        std::ofstream script(path_);
        script << "#!/bin/sh\n"
                  "if [ \"$1\" = \"-hide_banner\" ]; then exit 0; fi\n"
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

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

TEST(TranscoderSupervisor, PublishLifecycleStartsAndStopsIndependentWorker) {
    FakeEncoder encoder;
    const auto preset_path = std::filesystem::temp_directory_path() /
                             ("streamforge-supervisor-" + std::to_string(::getpid()) + ".conf");
    {
        std::ofstream presets(preset_path);
        presets << "football/live2|720p|live2_720p|default|h264|2500000|high|60|1280|720|"
                   "letterbox|aac|128000|first\n";
    }
    auto catalogue = rtmp_server::transcoding::PresetCatalogue::load(preset_path.string());
    ASSERT_TRUE(catalogue.ok());

    rtmp_server::transcoding::SupervisorOptions options;
    options.enabled = true;
    options.ffmpeg_path = encoder.string();
    options.max_active_jobs = 1;
    options.max_outputs_per_job = 2;
    options.stop_timeout = std::chrono::seconds(1);
    rtmp_server::transcoding::TranscoderSupervisor supervisor(options, std::move(catalogue).value());

    std::string prepared;
    supervisor.set_prepare_output([&prepared](std::string_view app, std::string_view stream) {
        prepared = std::string(app) + "/" + std::string(stream);
        return true;
    });
    ASSERT_TRUE(supervisor.start().ok());

    rtmp_server::protocol::commands::StreamRegistration registration;
    registration.app = "football";
    registration.stream_key = "live2";
    registration.connection_id = 42;
    supervisor.on_publish_started(registration);

    ASSERT_TRUE(eventually([&] {
        auto jobs = supervisor.snapshot();
        return jobs.size() == 1 && jobs.front().running;
    }));
    EXPECT_EQ(prepared, "football/live2_720p");
    EXPECT_TRUE(supervisor.is_managed_output("football", "live2_720p"));

    auto replacement = rtmp_server::transcoding::PresetCatalogue::parse(
        "football/live2|480p|live2_480p|default|h264|900000|main|60|854|480|"
        "letterbox|aac|96000|first\n");
    ASSERT_TRUE(replacement.ok());
    supervisor.apply_rule(replacement.value().rules().front());
    ASSERT_TRUE(eventually([&] {
        auto jobs = supervisor.snapshot();
        return jobs.size() == 1 && jobs.front().running &&
               jobs.front().output_streams == std::vector<std::string>{"live2_480p"};
    }, 3s));
    EXPECT_FALSE(supervisor.is_managed_output("football", "live2_720p"));
    EXPECT_TRUE(supervisor.is_managed_output("football", "live2_480p"));

    supervisor.on_publish_stopped(42);
    EXPECT_TRUE(eventually([&] { return supervisor.snapshot().empty(); }, 3s));
    EXPECT_FALSE(supervisor.is_managed_output("football", "live2_480p"));
    supervisor.stop();
    std::filesystem::remove(preset_path);
}

} // namespace
