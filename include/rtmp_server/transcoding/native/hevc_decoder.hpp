#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"

namespace rtmp_server::transcoding::native {

namespace detail {

// libde265 reports decode problems as a de265_error code. Keep the fatal/
// recoverable policy independent of the third-party enum type (same reason
// h264_decoder.hpp isolates OpenH264's DECODING_STATE bitmask) so it stays
// unit-testable without pulling <libde265/de265.h> into this header.
[[nodiscard]] bool libde265_decode_error_is_fatal(int error_code) noexcept;
[[nodiscard]] std::string libde265_decode_error_description(int error_code);

} // namespace detail

// Wraps a libde265 decoder. Input is Annex B (start-code prefixed) HEVC —
// convert HVCC samples with the equivalent hevc-to-annexb helper first (see
// media::h264::avcc_to_annexb for the H.264 analogue). Output is I420, copied
// into a caller-owned YuvFrame so the pipeline can reuse buffers, matching
// H264Decoder's shape so SourceTranscoder can select between the two with
// minimal branching.
//
// libde265 is a CPU HEVC decoder distributed under the LGPL v2.1. Unlike
// openh264/x265 (BSD-family), the LGPL requires dynamic linking to stay
// compliant when this server is redistributed as a combined work -- do not
// statically link libde265 into the server binary. The CMake wiring for this
// class links PkgConfig::LIBDE265, which resolves to the system's shared
// library via pkg-config, not a static archive.
class HevcDecoder {
public:
    HevcDecoder();
    ~HevcDecoder();
    HevcDecoder(const HevcDecoder&) = delete;
    HevcDecoder& operator=(const HevcDecoder&) = delete;

    [[nodiscard]] core::Result<void> initialize();

    // Feeds one Annex B access unit. On success `produced` is true when a full
    // frame was decoded into `out` (some access units -- e.g. parameter sets
    // only -- decode nothing). `pts_90k` is stamped onto the produced frame.
    [[nodiscard]] core::Result<void> decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                            YuvFrame& out, bool& produced);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtmp_server::transcoding::native
