#include "rtmp_server/transcoding/native/hevc_encoder.hpp"

#include <x265.h>

#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error encode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

bool is_keyframe(int slice_type) {
    return slice_type == X265_TYPE_IDR || slice_type == X265_TYPE_I;
}

} // namespace

struct HevcEncoder::Impl {
    x265_param* param = nullptr;
    x265_encoder* encoder = nullptr;

    ~Impl() {
        if (encoder) x265_encoder_close(encoder);
        if (param) x265_param_free(param);
    }
};

HevcEncoder::HevcEncoder() : impl_(std::make_unique<Impl>()) {}
HevcEncoder::~HevcEncoder() = default;

core::Result<void> HevcEncoder::open(const HevcParamSet& params) {
    impl_->param = x265_param_alloc();
    if (impl_->param == nullptr) return encode_error("x265_param_alloc failed");

    const char* tune = params.tune.empty() ? nullptr : params.tune.c_str();
    if (x265_param_default_preset(impl_->param, params.preset.c_str(), tune) != 0) {
        return encode_error("invalid x265 preset/tune: " + params.preset + "/" + params.tune);
    }

    impl_->param->sourceWidth = static_cast<int>(params.width);
    impl_->param->sourceHeight = static_cast<int>(params.height);
    impl_->param->fpsNum = params.fps_num;
    impl_->param->fpsDenom = params.fps_den;
    impl_->param->internalCsp = X265_CSP_I420;

    for (const auto& [key, value] : params.options) {
        if (x265_param_parse(impl_->param, key.c_str(), value.c_str()) != 0) {
            return encode_error("x265 rejected parameter " + key + "=" + value);
        }
    }

    impl_->encoder = x265_encoder_open(impl_->param);
    if (impl_->encoder == nullptr) return encode_error("x265_encoder_open failed");
    width_ = params.width;
    height_ = params.height;
    return {};
}

core::Result<void> HevcEncoder::encode(const YuvFrame& frame,
                                       std::vector<EncodedAccessUnit>& out) {
    if (impl_->encoder == nullptr) return encode_error("encoder not opened");
    if (frame.width != width_ || frame.height != height_) {
        return encode_error("frame geometry does not match encoder configuration");
    }

    x265_picture pic_in;
    x265_picture_init(impl_->param, &pic_in);
    pic_in.colorSpace = X265_CSP_I420;
    pic_in.bitDepth = 8;
    pic_in.pts = frame.pts_90k;
    pic_in.planes[0] = const_cast<std::uint8_t*>(frame.y.data());
    pic_in.planes[1] = const_cast<std::uint8_t*>(frame.u.data());
    pic_in.planes[2] = const_cast<std::uint8_t*>(frame.v.data());
    pic_in.stride[0] = frame.y_stride;
    pic_in.stride[1] = frame.u_stride;
    pic_in.stride[2] = frame.v_stride;

    x265_picture pic_out;
    x265_picture_init(impl_->param, &pic_out);
    x265_nal* nal = nullptr;
    std::uint32_t nal_count = 0;

    const int result =
        x265_encoder_encode(impl_->encoder, &nal, &nal_count, &pic_in, &pic_out);
    if (result < 0) return encode_error("x265_encoder_encode failed");
    if (result > 0 && nal_count > 0) {
        EncodedAccessUnit au;
        au.pts_90k = pic_out.pts;
        au.dts_90k = pic_out.dts;
        au.keyframe = is_keyframe(pic_out.sliceType);
        std::size_t total = 0;
        for (std::uint32_t i = 0; i < nal_count; ++i) total += nal[i].sizeBytes;
        au.annexb.resize(total);
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < nal_count; ++i) {
            std::memcpy(au.annexb.data() + offset, nal[i].payload, nal[i].sizeBytes);
            offset += nal[i].sizeBytes;
        }
        out.push_back(std::move(au));
    }
    return {};
}

core::Result<void> HevcEncoder::flush(std::vector<EncodedAccessUnit>& out) {
    if (impl_->encoder == nullptr) return {};
    for (;;) {
        x265_picture pic_out;
        x265_picture_init(impl_->param, &pic_out);
        x265_nal* nal = nullptr;
        std::uint32_t nal_count = 0;
        const int result =
            x265_encoder_encode(impl_->encoder, &nal, &nal_count, nullptr, &pic_out);
        if (result < 0) return encode_error("x265 flush failed");
        if (result == 0 || nal_count == 0) break;
        EncodedAccessUnit au;
        au.pts_90k = pic_out.pts;
        au.dts_90k = pic_out.dts;
        au.keyframe = is_keyframe(pic_out.sliceType);
        std::size_t total = 0;
        for (std::uint32_t i = 0; i < nal_count; ++i) total += nal[i].sizeBytes;
        au.annexb.resize(total);
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < nal_count; ++i) {
            std::memcpy(au.annexb.data() + offset, nal[i].payload, nal[i].sizeBytes);
            offset += nal[i].sizeBytes;
        }
        out.push_back(std::move(au));
    }
    return {};
}

} // namespace rtmp_server::transcoding::native
