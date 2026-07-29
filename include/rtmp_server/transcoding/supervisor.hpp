#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/transcoding/backend.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding {

struct SupervisorOptions {
    bool enabled = false;
    std::string loopback_host = "127.0.0.1";
    std::uint16_t rtmp_port = 1935;
    std::string ffmpeg_path = "/usr/bin/ffmpeg";
    std::size_t max_active_jobs = 16;
    std::size_t max_outputs_per_job = 8;
    std::chrono::seconds stop_timeout{5};
    std::chrono::seconds restart_delay{2};
    std::uint32_t max_restart_attempts = 5;
};

struct JobSnapshot {
    std::uint64_t source_connection_id = 0;
    std::string application;
    std::string source_stream;
    std::vector<std::string> output_streams;
    int process_id = -1;
    std::uint32_t restart_attempts = 0;
    bool running = false;
};

// Owns no media buffers. It turns publish lifecycle events into commands for
// one control thread, which starts/stops independent codec processes. The
// RTMP event-loop threads only enqueue small values and return immediately.
class TranscoderSupervisor {
public:
    using PrepareOutput = std::function<bool(std::string_view application,
                                              std::string_view output_stream)>;
    using RenditionsReady = std::function<void(std::string_view application,
                                                std::string_view source_stream,
                                                const std::vector<Preset>& presets)>;

    TranscoderSupervisor(SupervisorOptions options, PresetCatalogue catalogue);
    ~TranscoderSupervisor();
    TranscoderSupervisor(const TranscoderSupervisor&) = delete;
    TranscoderSupervisor& operator=(const TranscoderSupervisor&) = delete;

    void set_prepare_output(PrepareOutput callback);
    void set_renditions_ready(RenditionsReady callback);

    [[nodiscard]] core::Result<void> start();
    void stop() noexcept;

    void on_publish_started(const protocol::commands::StreamRegistration& registration);
    void on_publish_stopped(std::uint64_t source_connection_id);
    void apply_rule(Rule rule);
    void remove_rule(std::string application, std::string source_stream);

    [[nodiscard]] bool is_managed_output(std::string_view application,
                                         std::string_view stream) const;
    [[nodiscard]] std::vector<JobSnapshot> snapshot() const;
    [[nodiscard]] std::vector<BackendCapabilities> capabilities() const;

private:
    enum class CommandKind { Start, Stop, ApplyRule, RemoveRule, Shutdown };
    struct Command {
        CommandKind kind = CommandKind::Start;
        protocol::commands::StreamRegistration registration;
        std::uint64_t connection_id = 0;
        std::optional<Rule> rule;
        std::string application;
        std::string source_stream;
    };
    struct Job {
        protocol::commands::StreamRegistration source;
        std::vector<Preset> presets;
        std::vector<std::string> argv;
        int pid = -1;
        std::uint32_t restart_attempts = 0;
        bool stop_requested = false;
        bool restart_after_stop = false;
        std::chrono::steady_clock::time_point restart_at{};
        std::optional<std::chrono::steady_clock::time_point> terminate_sent_at;
    };

    void run();
    void handle_start(const protocol::commands::StreamRegistration& registration);
    void handle_stop(std::uint64_t connection_id);
    void handle_apply_rule(Rule rule);
    void handle_remove_rule(std::string_view application, std::string_view source_stream);
    void spawn(Job& job);
    void reap_children();
    void enforce_stop_deadlines();
    void stop_all_children();
    [[nodiscard]] core::Result<std::vector<std::string>>
    build_arguments(const protocol::commands::StreamRegistration& source,
                    const std::vector<Preset>& presets) const;

    SupervisorOptions options_;
    PresetCatalogue catalogue_;
    BackendRegistry backends_;
    PrepareOutput prepare_output_;
    RenditionsReady renditions_ready_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::unordered_map<std::uint64_t, Job> jobs_;
    std::unordered_set<std::string> managed_outputs_;
    std::thread thread_;
    bool started_ = false;
    bool stopping_ = false;
};

} // namespace rtmp_server::transcoding
