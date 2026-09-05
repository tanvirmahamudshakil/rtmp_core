#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/dispatch/transcoder_dispatch_manager.hpp"

using namespace rtmp_server;
using rtmp_server::cluster::NodeHeartbeat;
using rtmp_server::cluster::NodeRegistry;
using rtmp_server::cluster::NodeRole;
using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;
using rtmp_server::control::HttpServer;
using rtmp_server::control::HttpServerOptions;
using rtmp_server::core::ErrorCode;
using rtmp_server::dispatch::DispatchedRendition;
using rtmp_server::dispatch::TranscoderDispatchManager;
using rtmp_server::dispatch::TranscoderJobConfig;
using rtmp_server::dispatch::TranscoderJobState;

namespace {

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
    core::Result<void> upsert_cluster_node(const persistence::ClusterNodeRow& row) override {
        nodes[row.id] = row;
        return {};
    }
    core::Result<std::vector<persistence::ClusterNodeRow>> load_cluster_nodes() override {
        std::vector<persistence::ClusterNodeRow> rows;
        for (const auto& [id, row] : nodes) rows.push_back(row);
        return rows;
    }

    core::Result<void> upsert_transcoder_job(const persistence::TranscoderJobRow& row) override {
        jobs[row.application + "/" + row.name] = row;
        return {};
    }
    core::Result<void> delete_transcoder_job(std::string_view application,
                                             std::string_view name) override {
        jobs.erase(std::string(application) + "/" + std::string(name));
        return {};
    }
    core::Result<std::vector<persistence::TranscoderJobRow>> load_transcoder_jobs() override {
        std::vector<persistence::TranscoderJobRow> rows;
        for (const auto& [key, row] : jobs) rows.push_back(row);
        return rows;
    }

    std::unordered_map<std::string, persistence::ClusterNodeRow> nodes;
    std::unordered_map<std::string, persistence::TranscoderJobRow> jobs;
};

// A fake transcoder_agent: accepts POST /jobs and DELETE /jobs/<id>, records
// what it was asked to run so tests can assert on the wire protocol.
class FakeAgent {
public:
    FakeAgent() {
        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        options.port = 0;
        server_ = std::make_unique<HttpServer>(options);
        server_->set_handler([this](const HttpRequest& request) { return handle(request); });
        started_ = server_->start();
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return server_->bound_port(); }
    void reject_next() { reject_ = true; }

    [[nodiscard]] std::vector<std::string> job_post_bodies() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return job_post_bodies_;
    }
    [[nodiscard]] std::vector<std::string> deleted_job_ids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deleted_job_ids_;
    }

private:
    HttpResponse handle(const HttpRequest& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.method == "POST" && request.path == "/jobs") {
            if (reject_) {
                reject_ = false;
                return HttpResponse::json(409, R"({"error":"busy"})");
            }
            job_post_bodies_.push_back(request.body);
            return HttpResponse::json(201, R"({"accepted":true})");
        }
        if (request.method == "DELETE" && request.path.starts_with("/jobs/")) {
            deleted_job_ids_.push_back(request.path.substr(6));
            return HttpResponse::json(200, R"({"deleted":true})");
        }
        return HttpResponse::json(404, R"({"error":"not found"})");
    }

    std::unique_ptr<HttpServer> server_;
    bool started_ = false;
    mutable std::mutex mutex_;
    bool reject_ = false;
    std::vector<std::string> job_post_bodies_;
    std::vector<std::string> deleted_job_ids_;
};

TranscoderJobConfig job_config(std::string name, std::string source_url = "rtmp://source.example.com/live/cam") {
    TranscoderJobConfig config;
    config.application = "live";
    config.name = std::move(name);
    config.source_url = std::move(source_url);
    config.fps = 30;
    DispatchedRendition hd;
    hd.name = "hd";
    hd.output_stream = config.name + "_720p";
    hd.width = 1280;
    hd.height = 720;
    hd.video_bitrate = 2'500'000;
    hd.audio_bitrate = 128'000;
    DispatchedRendition sd;
    sd.name = "sd";
    sd.output_stream = config.name + "_480p";
    sd.width = 854;
    sd.height = 480;
    sd.video_bitrate = 900'000;
    sd.audio_bitrate = 96'000;
    config.renditions = {hd, sd};
    return config;
}

TEST(TranscoderDispatchManagerTest, RejectsAJobWithNoRenditions) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager manager(&registry, &store, {});
    auto config = job_config("main");
    config.renditions.clear();
    auto result = manager.create(config, 1'000);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidConfiguration);
}

TEST(TranscoderDispatchManagerTest, RejectsARenditionThatReusesTheJobName) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager manager(&registry, &store, {});
    auto config = job_config("main");
    config.renditions.front().output_stream = "main";
    auto result = manager.create(config, 1'000);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("job name"), std::string::npos);
}

TEST(TranscoderDispatchManagerTest, RejectsTwoRenditionsSharingOneOutputStream) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager manager(&registry, &store, {});
    auto config = job_config("main");
    config.renditions[1].output_stream = config.renditions[0].output_stream;
    auto result = manager.create(config, 1'000);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("share one output_stream"), std::string::npos);
}

TEST(TranscoderDispatchManagerTest, IsUnassignableWithNoHealthyTranscoderNode) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager manager(&registry, &store, {});
    auto status = manager.create(job_config("main"), 1'000);
    ASSERT_TRUE(status) << status.error().message();
    EXPECT_EQ(status.value().state, TranscoderJobState::Unassignable);
    EXPECT_TRUE(status.value().assigned_node_id.empty());
}

// End to end: a real transcoder node registers by heartbeat, the manager
// dispatches the job to it over real HTTP, and the wire body carries exactly
// the renditions the job was created with.
TEST(TranscoderDispatchManagerTest, DispatchesToTheLeastLoadedHealthyTranscoder) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());

    FakeStore store;
    NodeRegistry registry(&store, {});
    NodeHeartbeat beat;
    beat.id = "transcoder-1";
    beat.role = NodeRole::Transcoder;
    beat.address = "127.0.0.1";
    ASSERT_TRUE(registry.heartbeat(beat, 1'000));

    TranscoderDispatchManager::Options options;
    options.agent_port = agent.port();
    TranscoderDispatchManager manager(&registry, &store, {}, options);

    auto status = manager.create(job_config("main"), 1'000);
    ASSERT_TRUE(status) << status.error().message();
    EXPECT_EQ(status.value().state, TranscoderJobState::Assigning);
    EXPECT_EQ(status.value().assigned_node_id, "transcoder-1");

    // Named, not chained: agent.job_post_bodies() returns a temporary vector,
    // and binding a reference to .front() of an unnamed temporary does NOT
    // extend that temporary's lifetime (lifetime extension only applies when
    // a reference binds directly to the temporary itself, not to a value
    // returned by a further member-function call on it) -- exactly the
    // dangling-reference footgun split_annexb_nal_units's own header comment
    // warns about, just self-inflicted here instead of in a library API.
    const auto job_posts = agent.job_post_bodies();
    ASSERT_EQ(job_posts.size(), 1u);
    const auto& body = job_posts.front();
    EXPECT_NE(body.find(R"("source_url":"rtmp://source.example.com/live/cam")"), std::string::npos);
    EXPECT_NE(body.find(R"("output_stream":"main_720p")"), std::string::npos);
    EXPECT_NE(body.find(R"("output_stream":"main_480p")"), std::string::npos);

    EXPECT_TRUE(manager.is_expected_publish("live", "main_720p"));
    EXPECT_TRUE(manager.is_expected_publish("live", "main_480p"));
    EXPECT_FALSE(manager.is_expected_publish("live", "someone_elses_stream"));
}

TEST(TranscoderDispatchManagerTest, TracksJobStateAsRenditionsPublishAndGoQuiet) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());
    FakeStore store;
    NodeRegistry registry(&store, {});
    NodeHeartbeat beat;
    beat.id = "transcoder-1";
    beat.role = NodeRole::Transcoder;
    beat.address = "127.0.0.1";
    ASSERT_TRUE(registry.heartbeat(beat, 1'000));

    TranscoderDispatchManager::Options options;
    options.agent_port = agent.port();
    TranscoderDispatchManager manager(&registry, &store, {}, options);
    ASSERT_TRUE(manager.create(job_config("main"), 1'000));

    manager.note_publish_state("live", "main_720p", true);
    ASSERT_EQ(manager.list("live").size(), 1u);
    EXPECT_EQ(manager.list("live").front().state, TranscoderJobState::Running);

    manager.note_publish_state("live", "main_480p", true);
    manager.note_publish_state("live", "main_720p", false);
    // One rendition still live: the job stays Running.
    EXPECT_EQ(manager.list("live").front().state, TranscoderJobState::Running);

    manager.note_publish_state("live", "main_480p", false);
    // Every rendition quiet now: the job is Lost, not silently forgotten.
    EXPECT_EQ(manager.list("live").front().state, TranscoderJobState::Lost);
}

TEST(TranscoderDispatchManagerTest, RemoveTellsTheAgentAndForgetsTheJob) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());
    FakeStore store;
    NodeRegistry registry(&store, {});
    NodeHeartbeat beat;
    beat.id = "transcoder-1";
    beat.role = NodeRole::Transcoder;
    beat.address = "127.0.0.1";
    ASSERT_TRUE(registry.heartbeat(beat, 1'000));

    TranscoderDispatchManager::Options options;
    options.agent_port = agent.port();
    TranscoderDispatchManager manager(&registry, &store, {}, options);
    ASSERT_TRUE(manager.create(job_config("main"), 1'000));

    ASSERT_TRUE(manager.remove("live", "main"));
    EXPECT_TRUE(manager.list("live").empty());
    EXPECT_FALSE(manager.is_expected_publish("live", "main_720p"));
    // The job id ("application/name") is sent as-is after "/jobs/" -- the
    // agent's route is "everything past the prefix is the id", not a
    // multi-segment REST path, so no percent-encoding is needed or done.
    ASSERT_EQ(agent.deleted_job_ids().size(), 1u);
    EXPECT_EQ(agent.deleted_job_ids().front(), "live/main");

    auto missing = manager.remove("live", "main");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

// The manager must never silently drop a job it could not place: it stays
// Unassignable and retry_unassigned() places it the moment a transcoder
// becomes available, mirroring a source-transcode job's own auto-restart.
TEST(TranscoderDispatchManagerTest, RetryUnassignedPlacesAJobOnceATranscoderAppears) {
    // Start the fake agent first so its ephemeral port is known before the
    // manager (which fixes the port at construction) is built.
    FakeAgent agent;
    ASSERT_TRUE(agent.started());

    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager::Options options;
    options.agent_port = agent.port();
    TranscoderDispatchManager manager(&registry, &store, {}, options);

    // No transcoder node has heartbeated yet: the job cannot be placed.
    ASSERT_TRUE(manager.create(job_config("main"), 1'000));
    EXPECT_EQ(manager.list("live").front().state, TranscoderJobState::Unassignable);
    EXPECT_TRUE(agent.job_post_bodies().empty());

    NodeHeartbeat beat;
    beat.id = "transcoder-1";
    beat.role = NodeRole::Transcoder;
    beat.address = "127.0.0.1";
    ASSERT_TRUE(registry.heartbeat(beat, 2'000));

    manager.retry_unassigned(2'000);
    EXPECT_EQ(manager.list("live").front().state, TranscoderJobState::Assigning);
    EXPECT_EQ(manager.list("live").front().assigned_node_id, "transcoder-1");
    ASSERT_EQ(agent.job_post_bodies().size(), 1u);
}

TEST(TranscoderDispatchManagerTest, RejectsADuplicateJobName) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    TranscoderDispatchManager manager(&registry, &store, {});
    ASSERT_TRUE(manager.create(job_config("main"), 1'000));
    auto duplicate = manager.create(job_config("main"), 1'000);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::Conflict);
}

TEST(TranscoderDispatchManagerTest, RebuildsExpectedPublishesFromTheStoreOnRestart) {
    FakeStore store;
    {
        NodeRegistry registry(&store, {});
        TranscoderDispatchManager manager(&registry, &store, {});
        ASSERT_TRUE(manager.create(job_config("main"), 1'000));
    }
    NodeRegistry registry2(&store, {});
    TranscoderDispatchManager restarted(&registry2, &store, {});
    restarted.load_from_store();
    EXPECT_TRUE(restarted.is_expected_publish("live", "main_720p"));
    EXPECT_TRUE(restarted.is_expected_publish("live", "main_480p"));
    ASSERT_EQ(restarted.list("live").size(), 1u);
    EXPECT_EQ(restarted.list("live").front().state, TranscoderJobState::Unassignable);
    ASSERT_EQ(restarted.list("live").front().renditions.size(), 2u);
}

} // namespace
