#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp" // EncodedAccessUnit

namespace rtmp_server::transcoding::native {

// Low-latency H.264 encoder configuration. Defaults target real-time delivery:
// libx264's "veryfast"/"zerolatency" preset/tune, zero B-frames (zero reorder
// delay) and slice-based (not frame-based) multithreading, so a frame goes in
// and an access unit comes out with sub-frame latency — no lookahead buffering.
struct H264EncoderConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
    // Bits/sec. Acts as the VBV ceiling (maxrate/bufsize), not a fixed target —
    // see `crf` below for why this yields the same quality at a lower actual
    // bitrate than strict CBR.
    std::uint32_t bitrate = 2'500'000;
    std::uint32_t gop = 60;            // intra period in frames (segment alignment)
    std::uint32_t threads = 1;         // encoder worker threads (per rendition)
    // Allow the encoder to skip frames to hold the target bitrate under load —
    // for live, dropping a frame beats letting latency grow.
    bool allow_frame_skip = true;
    // 0 = baseline (widest device support), 1 = main, 2 = high.
    int profile = 2;
    // Constant Rate Factor: the quality anchor (lower = better quality/more
    // bits). x264 spends only as many bits as the scene needs, up to the `bitrate`
    // VBV cap above, instead of always spending the full target like strict CBR
    // did under the previous openh264 backend. 23 is a visually-transparent-ish
    // default for live at 720p-1080p.
    double crf = 23.0;
};

// Builds a low-latency H264EncoderConfig from a transcoding preset at the given
// output geometry and frame rate. Pure and testable (no openh264 dependency).
[[nodiscard]] H264EncoderConfig build_h264_config(std::uint32_t out_w, std::uint32_t out_h,
                                                  std::uint32_t fps, std::uint32_t bitrate,
                                                  std::uint32_t gop, std::uint32_t threads);

// Wraps a libx264 encoder emitting Annex B access units. Decode still runs on
// openh264 (see H264Decoder); only the encode side is libx264 so this keeps
// the whole pipeline FFmpeg-process-free while gaining CABAC and x264's
// rate-distortion optimisation, which openh264 (CAVLC-only) cannot match at
// the same bitrate.
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
    std::uint32_t fps_ = 1;
    std::uint64_t submitted_frames_ = 0;
    bool pacing_clock_set_ = false;
    std::int64_t last_media_pts_90k_ = 0;
    std::uint64_t pacing_timestamp_ms_ = 0;
};

} // namespace rtmp_server::transcoding::native
