#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <memory>
#include <string>

#include "rtmp_server/transcoding/native/source_job_manager.hpp"

using namespace rtmp_server;
using namespace rtmp_server::transcoding::native;

namespace {

hls::SegmentPtr make_segment(std::uint64_t sequence) {
    auto segment = std::make_shared<hls::Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = std::chrono::seconds(2);
    segment->data = core::SharedBuffer::adopt(std::vector<std::byte>(188, std::byte{0x47}));
    return segment;
}

} // namespace

TEST(SourceJobManagerTest, ManualRestartPreservesRegisteredStoreAndSequence) {
    std::shared_ptr<hls::SegmentStore> registered_store;
    std::size_t registrations = 0;
    std::size_t unregistrations = 0;

    SourceJobManager::Hooks hooks;
    hooks.register_store = [&](const std::string&, const std::string&,
                               std::shared_ptr<hls::SegmentStore> store) {
        registered_store = std::move(store);
        ++registrations;
    };
    hooks.unregister_store = [&](const std::string&, const std::string&) { ++unregistrations; };
    SourceJobManager manager(std::move(hooks));

    SourceJobConfig config;
    config.application = "live";
    config.name = "demo";
    // Connection-refused fails quickly if the puller reaches the network;
    // the lifecycle assertions do not depend on a working upstream.
    config.source_url = "http://127.0.0.1:1/source.m3u8";
    config.template_name = "test";
    config.renditions.push_back(RenditionSpec{"480p", "demo_480p", 854, 480, 500'000, 60, 96'000});

    ASSERT_TRUE(manager.create(config).ok());
    ASSERT_NE(registered_store, nullptr);
    ASSERT_EQ(registrations, 1u);
    registered_store->add_segment(make_segment(17));

    ASSERT_TRUE(manager.restart("live", "demo").ok());
    EXPECT_EQ(registrations, 1u) << "restart must not swap the registered delivery store";
    EXPECT_EQ(unregistrations, 0u) << "restart must not make existing session URLs return 404";
    EXPECT_NE(registered_store->find_segment("segment-17.ts"), nullptr);
    EXPECT_EQ(registered_store->next_sequence(), 18u);
}

#endif
