#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/ts/ts_muxer.hpp"

// Minimal MPEG-2 Transport Stream demultiplexer — the inverse of TsMuxer.
// Sufficient for pulling an HLS/TS source that carries H.264 (Annex B) video
// and ADTS AAC audio back into elementary access units, with no transcoding
// and no external dependency. Deliberately symmetric with ts_muxer.hpp:
// same 90 kHz clock, same H.264/AAC stream types (0x1B / 0x0F).
//
// It is tolerant of real-world TS: it resynchronises on the 0x47 sync byte,
// skips adaptation fields, follows the PAT to the first program's PMT, and
// reassembles PES packets that span many TS packets. Access units are
// delimited by the payload_unit_start_indicator, so a video PES with the
// "unbounded" length 0 (routine for video) is handled correctly.
namespace rtmp_server::media::ts {

// Which codec was found on the video PID (from the PMT stream_type). Routing
// of PES payload through to the video handler is identical either way — this
// exists only so callers (e.g. the native source-transcode pipeline) know
// which decoder to feed the emitted access units to.
enum class TsVideoCodec { Unknown, H264, Hevc };

class TsDemuxer {
public:
    // annexb is a complete H.264 access unit (start-code prefixed). keyframe is
    // set when the access unit contains an IDR NAL. Timestamps are 90 kHz.
    using VideoHandler = std::function<void(std::span<const std::byte> annexb, std::uint64_t pts_90k,
                                            std::uint64_t dts_90k, bool keyframe)>;
    // adts is one PES payload of ADTS-framed AAC (one or more frames).
    using AudioHandler =
        std::function<void(std::span<const std::byte> adts, std::uint64_t pts_90k)>;

    void set_video_handler(VideoHandler handler) { video_handler_ = std::move(handler); }
    void set_audio_handler(AudioHandler handler) { audio_handler_ = std::move(handler); }

    // Feeds an arbitrary run of TS bytes (e.g. one downloaded segment). Any
    // trailing partial packet is buffered until the next feed().
    [[nodiscard]] core::Result<void> feed(std::span<const std::byte> ts_bytes);

    // Emits any access unit still buffered — call at end of a segment/stream so
    // the final PES (which has no following PUSI to close it) is not lost.
    void flush();

    // Discards all reassembly and program state (e.g. on a source restart).
    void reset() noexcept;

    // The video codec detected on the PMT's video elementary stream (H.264
    // stream_type 0x1B or HEVC stream_type 0x24). Unknown until the PMT has
    // been parsed and a supported video stream was found.
    [[nodiscard]] TsVideoCodec video_codec() const noexcept { return video_codec_; }

private:
    enum class ElementaryKind { None, Video, Audio };

    struct PesAssembly {
        ElementaryKind kind = ElementaryKind::None;
        std::vector<std::byte> data;   // accumulated ES payload
        std::uint64_t pts_90k = 0;
        std::uint64_t dts_90k = 0;
        bool has_data = false;
    };

    [[nodiscard]] core::Result<void> process_packet(std::span<const std::byte> packet);
    void parse_pat(std::span<const std::byte> section);
    void parse_pmt(std::span<const std::byte> section);
    void begin_pes(PesAssembly& asm_state, std::span<const std::byte> payload);
    void finish_pes(PesAssembly& asm_state);

    VideoHandler video_handler_;
    AudioHandler audio_handler_;

    std::uint16_t pmt_pid_ = 0xFFFF; // unknown until the PAT is seen
    std::uint16_t video_pid_ = 0xFFFF;
    std::uint16_t audio_pid_ = 0xFFFF;
    bool pmt_known_ = false;
    TsVideoCodec video_codec_ = TsVideoCodec::Unknown;

    PesAssembly video_;
    PesAssembly audio_;
    std::vector<std::byte> partial_; // leftover bytes < one packet across feeds
};

} // namespace rtmp_server::media::ts
