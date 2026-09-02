#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "rtmp_server/dash/mpd.hpp"

using namespace rtmp_server::dash;

namespace {

Representation video_rep(const std::string& id, std::uint64_t bandwidth) {
    Representation rep;
    rep.id = id;
    rep.bandwidth = bandwidth;
    rep.codecs = "avc1.42001e,mp4a.40.2";
    rep.mime_type = "video/mp4";
    rep.width = 1280;
    rep.height = 720;
    rep.frame_rate = 30.0;
    rep.init_template = "{rep}/init.mp4";
    rep.media_template = "{rep}/chunk-$Number$.m4s";
    return rep;
}

} // namespace

TEST(MpdTest, LiveManifestDeclaresDynamicTypeAndSegmentTemplate) {
    std::vector<Representation> reps{video_rep("main", 3'000'000)};
    MpdOptions options;
    options.timescale = 90000;
    options.segment_duration = 540000; // 6s at 90kHz
    options.start_number = 3;

    const auto mpd = build_mpd(reps, options);
    EXPECT_NE(mpd.find("type=\"dynamic\""), std::string::npos);
    EXPECT_NE(mpd.find("<Representation id=\"main\" bandwidth=\"3000000\""), std::string::npos);
    EXPECT_NE(mpd.find("codecs=\"avc1.42001e,mp4a.40.2\""), std::string::npos);
    EXPECT_NE(mpd.find("width=\"1280\" height=\"720\""), std::string::npos);
    EXPECT_NE(mpd.find("timescale=\"90000\""), std::string::npos);
    EXPECT_NE(mpd.find("duration=\"540000\""), std::string::npos);
    EXPECT_NE(mpd.find("startNumber=\"3\""), std::string::npos);
    EXPECT_NE(mpd.find("initialization=\"main/init.mp4\""), std::string::npos);
    EXPECT_NE(mpd.find("media=\"main/chunk-$Number$.m4s\""), std::string::npos);
    // The literal token a player substitutes must survive verbatim, not get
    // XML-escaped into something a URL template parser would reject.
    EXPECT_NE(mpd.find("$Number$"), std::string::npos);
}

TEST(MpdTest, StaticManifestDeclaresDurationAndNoUpdatePeriod) {
    std::vector<Representation> reps{video_rep("main", 3'000'000)};
    MpdOptions options;
    options.is_static = true;
    options.total_duration_seconds = 125.5;
    options.minimum_update_period_seconds = 4.0; // must be ignored for a static MPD

    const auto mpd = build_mpd(reps, options);
    EXPECT_NE(mpd.find("type=\"static\""), std::string::npos);
    EXPECT_NE(mpd.find("mediaPresentationDuration=\"PT125.500S\""), std::string::npos);
    EXPECT_EQ(mpd.find("minimumUpdatePeriod"), std::string::npos);
}

TEST(MpdTest, RepresentationsAreSortedLowestBandwidthFirstWithinTheirAdaptationSet) {
    std::vector<Representation> reps{video_rep("high", 5'000'000), video_rep("low", 800'000)};
    const auto mpd = build_mpd(reps, {});
    EXPECT_LT(mpd.find("id=\"low\""), mpd.find("id=\"high\""));
}

TEST(MpdTest, DistinctMimeTypesProduceSeparateAdaptationSets) {
    auto video = video_rep("video", 3'000'000);
    Representation audio;
    audio.id = "audio";
    audio.bandwidth = 128'000;
    audio.mime_type = "audio/mp4";
    audio.audio_sampling_rate = 44100;
    audio.init_template = "{rep}/init.mp4";
    audio.media_template = "{rep}/chunk-$Number$.m4s";

    std::vector<Representation> reps{video, audio};
    const auto mpd = build_mpd(reps, {});
    EXPECT_EQ(std::count(mpd.begin(), mpd.end(), '\0'), 0); // sanity: valid string
    const auto video_adaptation = mpd.find("contentType=\"video\"");
    const auto audio_adaptation = mpd.find("contentType=\"audio\"");
    ASSERT_NE(video_adaptation, std::string::npos);
    ASSERT_NE(audio_adaptation, std::string::npos);
    EXPECT_NE(mpd.find("audioSamplingRate=\"44100\""), std::string::npos);
    // width/height/frameRate are video-only attributes; an audio
    // representation must not carry them even if the struct happens to be
    // zero-initialised (nothing to accidentally emit).
    const auto audio_rep_line = mpd.find("id=\"audio\"");
    ASSERT_NE(audio_rep_line, std::string::npos);
    const auto audio_rep_end = mpd.find('\n', audio_rep_line);
    // A bare "width=\"" false-positives on "band**width**=\"..."; require the
    // space that only precedes the real attribute.
    EXPECT_EQ(mpd.substr(audio_rep_line, audio_rep_end - audio_rep_line).find(" width=\""),
              std::string::npos);
}

TEST(MpdTest, AttributeValuesAreXmlEscaped) {
    auto rep = video_rep("m<a>in\"&\"", 1'000'000);
    std::vector<Representation> reps{rep};
    const auto mpd = build_mpd(reps, {});
    EXPECT_EQ(mpd.find("m<a>in"), std::string::npos); // raw '<' would break the document
    EXPECT_NE(mpd.find("m&lt;a&gt;in&quot;&amp;&quot;"), std::string::npos);
}

TEST(MpdTest, EmptyRepresentationListProducesAWellFormedEmptyManifest) {
    const auto mpd = build_mpd({}, {});
    EXPECT_NE(mpd.find("<MPD"), std::string::npos);
    EXPECT_NE(mpd.find("</MPD>"), std::string::npos);
    EXPECT_EQ(mpd.find("<AdaptationSet"), std::string::npos);
}
