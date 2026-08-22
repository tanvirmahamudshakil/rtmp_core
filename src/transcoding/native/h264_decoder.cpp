#include "rtmp_server/transcoding/native/h264_decoder.hpp"

#include <wels/codec_api.h>

#include <chrono>
#include <cstring>
#include <sstream>

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

// Copies one openh264 output plane (which may be strided) into a tightly packed
// destination sized width x height.
void copy_plane(std::uint8_t* dst, int dst_stride, const std::uint8_t* src, int src_stride,
                std::uint32_t width, std::uint32_t height) {
    const auto dst_pitch = static_cast<std::size_t>(dst_stride);
    const auto src_pitch = static_cast<std::size_t>(src_stride);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + row * dst_pitch, src + row * src_pitch, width);
    }
}

} // namespace

struct H264Decoder::Impl {
    ISVCDecoder* decoder = nullptr;
    std::uint32_t last_recoverable_state = 0;
    std::uint64_t consecutive_recoverable_states = 0;
    std::uint64_t suppressed_recoverable_logs = 0;
    std::chrono::steady_clock::time_point last_recoverable_log{};

    ~Impl() {
        if (decoder) {
            decoder->Uninitialize();
            WelsDestroyDecoder(decoder);
        }
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

core::Result<void> H264Decoder::initialize() {
    if (WelsCreateDecoder(&impl_->decoder) != 0 || impl_->decoder == nullptr) {
        return decode_error("failed to create openh264 decoder");
    }
    SDecodingParam param{};
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY; // conceal rather than drop on loss
    if (impl_->decoder->Initialize(&param) != 0) {
        return decode_error("failed to initialize openh264 decoder");
    }
    return {};
}

core::Result<void> H264Decoder::decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                       YuvFrame& out, bool& produced) {
    produced = false;
    if (impl_->decoder == nullptr) return decode_error("decoder not initialized");

    std::uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    SBufferInfo info{};
    // OpenH264 may delay output while reordering a source that contains
    // B-frames. Associate the input access unit's PTS with the decoded
    // picture through the decoder instead of stamping whichever input PTS
    // happened to produce output. The latter creates non-monotonic DTS/PTS
    // in the transcoded MPEG-TS stream.
    info.uiInBsTimeStamp = static_cast<unsigned long long>(pts_90k);
    const auto* data = reinterpret_cast<const unsigned char*>(annexb.data());
    const DECODING_STATE state =
        impl_->decoder->DecodeFrameNoDelay(data, static_cast<int>(annexb.size()), planes, &info);
    const auto state_value = static_cast<std::uint32_t>(state);
    if (detail::openh264_decode_state_is_fatal(state_value)) {
        return decode_error("openh264 fatal decode state " + std::to_string(state_value) + " (" +
                            detail::openh264_decode_state_description(state_value) + ")");
    }

    if (state != dsErrorFree) {
        // OpenH264's bitstream-level states are deliberately recoverable: a
        // live source may begin between IDRs, lose a reference picture, or
        // conceal damaged data and still produce a valid I420 frame. The API
        // explicitly makes iBufferStatus authoritative for output validity.
        // Turning any non-zero bit into a pipeline error restarted the entire
        // source job on one imperfect access unit and froze every viewer.
        ++impl_->consecutive_recoverable_states;
        const auto now = std::chrono::steady_clock::now();
        const bool state_changed = state_value != impl_->last_recoverable_state;
        const bool log_due = impl_->last_recoverable_log == std::chrono::steady_clock::time_point{} ||
                             now - impl_->last_recoverable_log >= std::chrono::seconds(5);
        if (state_changed || log_due) {
            RTMP_LOG(LogLevel::Warn, "openh264_decoder", "recoverable_decode_state",
                     {{"state", std::to_string(state_value)},
                      {"flags", detail::openh264_decode_state_description(state_value)},
                      {"buffer_status", std::to_string(info.iBufferStatus)},
                      {"input_bytes", std::to_string(annexb.size())},
                      {"consecutive", std::to_string(impl_->consecutive_recoverable_states)},
                      {"suppressed_since_last_log",
                       std::to_string(impl_->suppressed_recoverable_logs)}});
            impl_->last_recoverable_log = now;
            impl_->last_recoverable_state = state_value;
            impl_->suppressed_recoverable_logs = 0;
        } else {
            ++impl_->suppressed_recoverable_logs;
        }
    } else {
        impl_->consecutive_recoverable_states = 0;
    }
    if (info.iBufferStatus != 1 || planes[0] == nullptr || planes[1] == nullptr ||
        planes[2] == nullptr) {
        return {}; // parameter-set-only access unit, or frame not ready yet
    }

    // A concealed picture is still a successful recovery point. Reset the
    // consecutive count once OpenH264 produced a complete frame so later log
    // records describe the current loss burst rather than process lifetime.
    impl_->consecutive_recoverable_states = 0;

    const auto& sys = info.UsrData.sSystemBuffer;
    const auto width = static_cast<std::uint32_t>(sys.iWidth);
    const auto height = static_cast<std::uint32_t>(sys.iHeight);
    if (width < 2 || height < 2) return decode_error("openh264 produced an invalid frame size");

    out.allocate(width, height);
    out.pts_90k = static_cast<std::int64_t>(info.uiOutYuvTimeStamp);
    const std::uint32_t chroma_w = (width + 1) / 2;
    const std::uint32_t chroma_h = (height + 1) / 2;
    copy_plane(out.y.data(), out.y_stride, planes[0], sys.iStride[0], width, height);
    copy_plane(out.u.data(), out.u_stride, planes[1], sys.iStride[1], chroma_w, chroma_h);
    copy_plane(out.v.data(), out.v_stride, planes[2], sys.iStride[1], chroma_w, chroma_h);
    produced = true;
    return {};
}

} // namespace rtmp_server::transcoding::native
