#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp" // EncodedAccessUnit

namespace rtmp_server::transcoding::native {

// Low-latency H.264 encoder configuration. Defaults target real-time delivery:
// openh264's CAMERA_VIDEO_REAL_TIME usage, low complexity, no B-frames (openh264
// has none, so there is zero reorder delay) and single-slice output, so a frame
// goes in and an access unit comes out with sub-frame latency.
struct H264EncoderConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
    std::uint32_t bitrate = 2'500'000; // bits/sec
    std::uint32_t gop = 60;            // intra period in frames (segment alignment)
    std::uint32_t threads = 1;         // encoder worker threads (per rendition)
    // Allow the encoder to skip frames to hold the target bitrate under load —
    // for live, dropping a frame beats letting latency grow.
    bool allow_frame_skip = true;
    // 0 = baseline (widest device support), 1 = main, 2 = high.
    int profile = 2;
};

// Builds a low-latency H264EncoderConfig from a transcoding preset at the given
// output geometry and frame rate. Pure and testable (no openh264 dependency).
[[nodiscard]] H264EncoderConfig build_h264_config(std::uint32_t out_w, std::uint32_t out_h,
                                                  std::uint32_t fps, std::uint32_t bitrate,
                                                  std::uint32_t gop, std::uint32_t threads);

// Wraps an openh264 encoder emitting Annex B access units. openh264 encodes as
// well as decodes, so choosing H.264 output keeps the whole pipeline
// FFmpeg-free and lets the existing H.264/AAC segmenter package the result
// unchanged.
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    [[nodiscard]] core::Result<void> open(const H264EncoderConfig& config);

    // Encodes one I420 frame. Appends an access unit to `out` unless the encoder
    // skipped the frame (rate control). `frame.pts_90k` is carried through.
    [[nodiscard]] core::Result<void> encode(const YuvFrame& frame,
                                            std::vector<EncodedAccessUnit>& out);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace rtmp_server::transcoding::native
