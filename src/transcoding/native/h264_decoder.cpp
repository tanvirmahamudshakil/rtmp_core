#include "rtmp_server/transcoding/native/h264_decoder.hpp"

// The runtime decoder is libavcodec's H.264 decoder: unlike OpenH264 (which
// only decodes Constrained Baseline reliably) it handles High / High 10 /
// 4:2:2 profiles, which is what essentially every IPTV and broadcast source
// actually ships. OpenH264's header is still included purely for the
// detail:: state-classification helpers below, which remain unit-tested.
#include <wels/codec_api.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::transcoding::native {

namespace detail {

namespace {

constexpr std::uint32_t state_bits(DECODING_STATE state) noexcept {
    return static_cast<std::uint32_t>(state);
}

constexpr std::uint32_t kRecoverableStateMask =
    state_bits(dsFramePending) | state_bits(dsRefLost) | state_bits(dsBitstreamError) |
    state_bits(dsDepLayerLost) | state_bits(dsNoParamSets) | state_bits(dsDataErrorConcealed) |
    state_bits(dsRefListNullPtrs);
constexpr std::uint32_t kFatalStateMask =
    state_bits(dsInvalidArgument) | state_bits(dsInitialOptExpected) | state_bits(dsOutOfMemory) |
    state_bits(dsDstBufNeedExpan);
constexpr std::uint32_t kKnownStateMask = kRecoverableStateMask | kFatalStateMask;

} // namespace

bool openh264_decode_state_is_fatal(std::uint32_t state) noexcept {
    return (state & kFatalStateMask) != 0 || (state & ~kKnownStateMask) != 0;
}

std::string openh264_decode_state_description(std::uint32_t state) {
    if (state == state_bits(dsErrorFree)) return "error-free";

    struct NamedState {
        std::uint32_t bit;
        const char* name;
    };
    constexpr NamedState states[] = {
        {state_bits(dsFramePending), "frame-pending"},
        {state_bits(dsRefLost), "reference-lost"},
        {state_bits(dsBitstreamError), "bitstream-error"},
        {state_bits(dsDepLayerLost), "dependency-layer-lost"},
        {state_bits(dsNoParamSets), "no-parameter-sets"},
        {state_bits(dsDataErrorConcealed), "data-error-concealed"},
        {state_bits(dsRefListNullPtrs), "null-reference-list"},
        {state_bits(dsInvalidArgument), "invalid-argument"},
        {state_bits(dsInitialOptExpected), "initialization-required"},
        {state_bits(dsOutOfMemory), "out-of-memory"},
        {state_bits(dsDstBufNeedExpan), "destination-buffer-too-small"},
    };

    std::ostringstream out;
    bool first = true;
    for (const auto& item : states) {
        if ((state & item.bit) == 0) continue;
        if (!first) out << '|';
        out << item.name;
        first = false;
    }
    if (const auto unknown = state & ~kKnownStateMask; unknown != 0) {
        if (!first) out << '|';
        out << "unknown-0x" << std::hex << unknown;
    }
    return out.str();
}

} // namespace detail

namespace {

using observability::LogLevel;

core::Error decode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

// Copies one decoded output plane (which may be strided) into a tightly packed
// destination sized width x height.
void copy_plane(std::uint8_t* dst, int dst_stride, const std::uint8_t* src, int src_stride,
                std::uint32_t width, std::uint32_t height) {
    const auto dst_pitch = static_cast<std::size_t>(dst_stride);
    const auto src_pitch = static_cast<std::size_t>(src_stride);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + row * dst_pitch, src + row * src_pitch, width);
    }
}

std::string av_err_string(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf[0] != '\0' ? std::string(buf) : ("averror " + std::to_string(errnum));
}

} // namespace

struct H264Decoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    // send_packet may hold a reference to the input past the call, so every
    // access unit is copied into this reusable, padding-extended buffer
    // rather than pointing libavcodec at the caller's span.
    std::vector<std::uint8_t> packet_buffer;

    std::uint64_t consecutive_recoverable_states = 0;
    std::uint64_t suppressed_recoverable_logs = 0;
    std::chrono::steady_clock::time_point last_recoverable_log{};

    // A live IPTV source routinely carries occasional corrupt access units;
    // libavcodec conceals and continues, and turning any decode error into a
    // pipeline failure would restart the whole job (and freeze every viewer)
    // on one bad packet. Log rate-limited, exactly as the OpenH264 path did.
    void note_recoverable(int rc, std::size_t input_bytes) {
        ++consecutive_recoverable_states;
        const auto now = std::chrono::steady_clock::now();
        const bool log_due = last_recoverable_log == std::chrono::steady_clock::time_point{} ||
                             now - last_recoverable_log >= std::chrono::seconds(5);
        if (log_due) {
            RTMP_LOG(LogLevel::Warn, "h264_decoder", "recoverable_decode_state",
                     {{"error", av_err_string(rc)},
                      {"input_bytes", std::to_string(input_bytes)},
                      {"consecutive", std::to_string(consecutive_recoverable_states)},
                      {"suppressed_since_last_log", std::to_string(suppressed_recoverable_logs)}});
            last_recoverable_log = now;
            suppressed_recoverable_logs = 0;
        } else {
            ++suppressed_recoverable_logs;
        }
    }

    ~Impl() {
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (ctx != nullptr) avcodec_free_context(&ctx);
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

core::Result<void> H264Decoder::initialize() {
    impl_->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (impl_->codec == nullptr) return decode_error("libavcodec: no H.264 decoder available");

    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (impl_->ctx == nullptr) return decode_error("libavcodec: failed to allocate H.264 context");

    // Input access units already carry timestamps on the 90 kHz clock; keep
    // them intact through the decoder so the produced frame's PTS stays on
    // the same timeline the rest of the transcode path uses.
    impl_->ctx->pkt_timebase = AVRational{1, 90000};
    impl_->ctx->time_base = AVRational{1, 90000};
    // Slice threading only: frame-level threading buffers thread_count frames
    // before emitting the first, which adds latency to a live path for no
    // throughput gain at these (downscaled) resolutions.
    impl_->ctx->thread_count = 1;
    impl_->ctx->thread_type = FF_THREAD_SLICE;
    // Tolerate the malformed bitstreams IPTV panels emit: conceal and carry
    // on rather than reject, mirroring the policy of the decoder this
    // replaces.
    impl_->ctx->err_recognition = 0;
    impl_->ctx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;

    if (const int rc = avcodec_open2(impl_->ctx, impl_->codec, nullptr); rc < 0) {
        return decode_error("libavcodec: avcodec_open2 failed: " + av_err_string(rc));
    }

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (impl_->packet == nullptr || impl_->frame == nullptr) {
        return decode_error("libavcodec: packet/frame allocation failed");
    }
    return {};
}

core::Result<void> H264Decoder::decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                       YuvFrame& out, bool& produced) {
    produced = false;
    if (impl_->ctx == nullptr) return decode_error("decoder not initialized");

    const std::size_t n = annexb.size();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(annexb.data());
    impl_->packet_buffer.assign(bytes, bytes + n);
    impl_->packet_buffer.resize(n + AV_INPUT_BUFFER_PADDING_SIZE, 0);

    av_packet_unref(impl_->packet);
    impl_->packet->data = impl_->packet_buffer.data();
    impl_->packet->size = static_cast<int>(n);
    impl_->packet->pts = pts_90k;
    impl_->packet->dts = pts_90k;

    if (const int rc = avcodec_send_packet(impl_->ctx, impl_->packet);
        rc < 0 && rc != AVERROR(EAGAIN)) {
        impl_->note_recoverable(rc, n);
        return {};
    }

    const int rc = avcodec_receive_frame(impl_->ctx, impl_->frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        // Parameter-set-only access unit, or the reorder buffer has not
        // filled yet -- both are ordinary, not errors.
        return {};
    }
    if (rc < 0) {
        impl_->note_recoverable(rc, n);
        return {};
    }

    const int fmt = impl_->frame->format;
    if (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) {
        av_frame_unref(impl_->frame);
        return decode_error("libavcodec produced pixel format " + std::to_string(fmt) +
                            " (only 8-bit 4:2:0 is supported by the transcode path)");
    }

    const auto width = static_cast<std::uint32_t>(impl_->frame->width);
    const auto height = static_cast<std::uint32_t>(impl_->frame->height);
    if (width < 2 || height < 2) {
        av_frame_unref(impl_->frame);
        return decode_error("libavcodec produced an invalid frame size");
    }

    impl_->consecutive_recoverable_states = 0;

    out.allocate(width, height);
    const std::int64_t ts = impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? impl_->frame->best_effort_timestamp
                            : impl_->frame->pts != AV_NOPTS_VALUE ? impl_->frame->pts
                                                                  : pts_90k;
    out.pts_90k = ts;
    const std::uint32_t chroma_w = (width + 1) / 2;
    const std::uint32_t chroma_h = (height + 1) / 2;
    copy_plane(out.y.data(), out.y_stride, impl_->frame->data[0], impl_->frame->linesize[0], width,
               height);
    copy_plane(out.u.data(), out.u_stride, impl_->frame->data[1], impl_->frame->linesize[1], chroma_w,
               chroma_h);
    copy_plane(out.v.data(), out.v_stride, impl_->frame->data[2], impl_->frame->linesize[2], chroma_w,
               chroma_h);
    av_frame_unref(impl_->frame);
    produced = true;
    return {};
}

} // namespace rtmp_server::transcoding::native
