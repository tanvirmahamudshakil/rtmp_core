#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "rtmp_server/dispatch/transcoder_agent.hpp"

using namespace rtmp_server;
using dispatch::TranscoderAgent;
using dispatch::TranscoderAgentOptions;
using dispatch::TranscoderJob;
using dispatch::TranscoderJobAssignment;
using dispatch::TranscoderJobRunnerStatus;

namespace {

struct RunnerCounts {
    std::atomic<int> created{0};
    std::atomic<int> stopped{0};
};

class FakeRunner final : public TranscoderJob {
public:
    FakeRunner(std::string id, std::shared_ptr<RunnerCounts> counts)
        : id_(std::move(id)), counts_(std::move(counts)) {
        ++counts_->created;
    }

    void stop() override {
        if (!stopped_.exchange(true)) ++counts_->stopped;
    }

    TranscoderJobRunnerStatus status() const override {
        return {.id = id_, .detail = "fake"};
    }

private:
    std::string id_;
    std::shared_ptr<RunnerCounts> counts_;
    std::atomic<bool> stopped_{false};
};

TranscoderJobAssignment assignment(std::string id = "live/main") {
    TranscoderJobAssignment value;
    value.id = std::move(id);
    value.source_url = "rtmp://source.example/live/input";
    value.fps = 30;
    value.target_application = "live";
    value.origin_rtmp_host = "origin.internal";
    value.origin_rtmp_port = 1935;
    value.renditions.push_back(
        {.name = "720p",
         .output_stream = "main_720p",
         .width = 1280,
         .height = 720,
         .video_bitrate = 2'500'000,
         .audio_bitrate = 128'000});
    return value;
}

TEST(TranscoderAgentParserTest, ParsesTheOriginWireShape) {
    auto parsed = dispatch::parse_transcoder_job_assignment(
        R"({"id":"live/main","source_url":"rtmp://source/live/input","fps":30,"target_application":"live","origin_rtmp_host":"10.0.0.4","origin_rtmp_port":1935,"renditions":[{"name":"720p","output_stream":"main_720p","width":1280,"height":720,"video_bitrate":2500000,"audio_bitrate":128000}]})");
    ASSERT_TRUE(parsed) << parsed.error().message();
    EXPECT_EQ(parsed.value().id, "live/main");
    EXPECT_EQ(parsed.value().origin_rtmp_port, 1935);
    ASSERT_EQ(parsed.value().renditions.size(), 1u);
    EXPECT_EQ(parsed.value().renditions.front().output_stream, "main_720p");
    EXPECT_EQ(parsed.value().renditions.front().width, 1280u);
}

TEST(TranscoderAgentParserTest, RejectsMalformedAndUnsafeAssignments) {
    EXPECT_FALSE(dispatch::parse_transcoder_job_assignment("not-json"));
    auto value = assignment("../main");
    auto counts = std::make_shared<RunnerCounts>();
    TranscoderAgent agent(
        [counts](TranscoderJobAssignment item) {
            return std::make_unique<FakeRunner>(std::move(item.id), counts);
        });
    auto result = agent.upsert(std::move(value));
    EXPECT_FALSE(result);
    EXPECT_EQ(counts->created.load(), 0);
}

TEST(TranscoderAgentTest, ExactRetryIsIdempotentAndChangedJobIsReplaced) {
    auto counts = std::make_shared<RunnerCounts>();
    TranscoderAgent agent(
        [counts](TranscoderJobAssignment value) {
            return std::make_unique<FakeRunner>(std::move(value.id), counts);
        });
    auto original = assignment();
    ASSERT_TRUE(agent.upsert(original));
    ASSERT_TRUE(agent.upsert(original));
    EXPECT_EQ(counts->created.load(), 1);
    EXPECT_EQ(counts->stopped.load(), 0);

    original.fps = 60;
    ASSERT_TRUE(agent.upsert(original));
    EXPECT_EQ(counts->created.load(), 2);
    EXPECT_EQ(counts->stopped.load(), 1);
    ASSERT_EQ(agent.list().size(), 1u);
    EXPECT_EQ(agent.list().front().id, "live/main");
}

TEST(TranscoderAgentTest, EnforcesCapacityAndStopsRemovedJobs) {
    auto counts = std::make_shared<RunnerCounts>();
    TranscoderAgent agent(
        [counts](TranscoderJobAssignment value) {
            return std::make_unique<FakeRunner>(std::move(value.id), counts);
        },
        TranscoderAgentOptions{.max_jobs = 1});
    ASSERT_TRUE(agent.upsert(assignment("live/one")));
    auto full = agent.upsert(assignment("live/two"));
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code(), core::ErrorCode::ResourceExhausted);
    ASSERT_TRUE(agent.remove("live/one"));
    EXPECT_EQ(counts->stopped.load(), 1);
    EXPECT_EQ(agent.size(), 0u);
    EXPECT_FALSE(agent.remove("live/missing"));
}

TEST(TranscoderAgentTest, ShutdownStopsEveryJobAndRejectsNewWork) {
    auto counts = std::make_shared<RunnerCounts>();
    TranscoderAgent agent(
        [counts](TranscoderJobAssignment value) {
            return std::make_unique<FakeRunner>(std::move(value.id), counts);
        });
    ASSERT_TRUE(agent.upsert(assignment("live/one")));
    ASSERT_TRUE(agent.upsert(assignment("live/two")));
    agent.stop_all();
    EXPECT_EQ(counts->stopped.load(), 2);
    EXPECT_EQ(agent.size(), 0u);
    EXPECT_FALSE(agent.upsert(assignment("live/three")));
}

} // namespace
