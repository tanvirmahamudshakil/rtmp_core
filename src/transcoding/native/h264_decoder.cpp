#include "rtmp_server/transcoding/native/h264_decoder.hpp"

#include <wels/codec_api.h>

#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

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
    if (state != dsErrorFree) {
        return decode_error("openh264 decode error");
    }
    if (info.iBufferStatus != 1 || planes[0] == nullptr) {
        return {}; // parameter-set-only access unit, or frame not ready yet
    }

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
