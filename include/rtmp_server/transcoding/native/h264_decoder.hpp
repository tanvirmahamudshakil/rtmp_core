#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"

namespace rtmp_server::transcoding::native {

namespace detail {

// OpenH264 returns DECODING_STATE as a bitmask. Keep the policy independent
// of the third-party enum type so it can be unit-tested without exposing a
// codec header through H264Decoder's public API.
[[nodiscard]] bool openh264_decode_state_is_fatal(std::uint32_t state) noexcept;
[[nodiscard]] std::string openh264_decode_state_description(std::uint32_t state);

} // namespace detail

// Wraps libavcodec's H.264 decoder. Input is Annex B (start-code prefixed)
// H.264 — convert AVCC samples with media::h264::avcc_to_annexb first. Output
// is 8-bit 4:2:0, copied into a caller-owned YuvFrame so the pipeline can
// reuse buffers.
//
// libavcodec (not OpenH264) is used because real IPTV/broadcast sources ship
// High profile — with 8x8 transform, custom scaling lists and CABAC — which
// OpenH264's decoder cannot handle (it rejects every access unit). The
// detail:: helpers below still classify OpenH264 DECODING_STATE bitmasks and
// keep their unit tests; they are no longer on the runtime path.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    [[nodiscard]] core::Result<void> initialize();

    // Feeds one Annex B access unit. On success `produced` is true when a full
    // frame was decoded into `out` (some access units — e.g. parameter sets
    // only — decode nothing). `pts_90k` is stamped onto the produced frame.
    [[nodiscard]] core::Result<void> decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                            YuvFrame& out, bool& produced);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtmp_server::transcoding::native
