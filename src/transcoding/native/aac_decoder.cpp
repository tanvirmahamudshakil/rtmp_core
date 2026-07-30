#include "rtmp_server/transcoding/native/aac_decoder.hpp"

#include <fdk-aac/aacdecoder_lib.h>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error decode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

} // namespace

struct AacDecoder::Impl {
    HANDLE_AACDECODER handle = nullptr;
    // libfdk decodes at most 2048 samples/channel/frame (HE-AAC), up to 8 ch.
    std::vector<INT_PCM> scratch = std::vector<INT_PCM>(2048 * 8);

    ~Impl() {
        if (handle) aacDecoder_Close(handle);
    }
};

AacDecoder::AacDecoder() : impl_(std::make_unique<Impl>()) {}
AacDecoder::~AacDecoder() = default;

core::Result<void> AacDecoder::configure(std::span<const std::byte> audio_specific_config) {
    if (audio_specific_config.empty()) return decode_error("empty AudioSpecificConfig");
    if (impl_->handle == nullptr) {
        // TT_MP4_RAW: we feed raw access units and supply the ASC out of band.
        impl_->handle = aacDecoder_Open(TT_MP4_RAW, 1);
        if (impl_->handle == nullptr) return decode_error("aacDecoder_Open failed");
    }
    auto* asc = reinterpret_cast<UCHAR*>(const_cast<std::byte*>(audio_specific_config.data()));
    UINT asc_len = static_cast<UINT>(audio_specific_config.size());
    if (aacDecoder_ConfigRaw(impl_->handle, &asc, &asc_len) != AAC_DEC_OK) {
        return decode_error("aacDecoder_ConfigRaw rejected the AudioSpecificConfig");
    }
    return {};
}

core::Result<void> AacDecoder::decode(std::span<const std::byte> aac_frame, PcmBlock& out,
                                      bool& produced) {
    produced = false;
    if (impl_->handle == nullptr) return decode_error("decoder not configured");

    auto* data = reinterpret_cast<UCHAR*>(const_cast<std::byte*>(aac_frame.data()));
    UINT size = static_cast<UINT>(aac_frame.size());
    UINT valid = size;
    if (aacDecoder_Fill(impl_->handle, &data, &size, &valid) != AAC_DEC_OK) {
        return decode_error("aacDecoder_Fill failed");
    }

    const AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(
        impl_->handle, impl_->scratch.data(), static_cast<INT>(impl_->scratch.size()), 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) return {}; // needs more input, not an error
    if (err != AAC_DEC_OK) return decode_error("aacDecoder_DecodeFrame failed");

    const CStreamInfo* info = aacDecoder_GetStreamInfo(impl_->handle);
    if (info == nullptr || info->sampleRate <= 0 || info->numChannels <= 0) {
        return decode_error("decoder produced invalid stream info");
    }
    const auto channels = static_cast<std::uint32_t>(info->numChannels);
    const auto frames = static_cast<std::uint32_t>(info->frameSize);
    const std::size_t count = static_cast<std::size_t>(frames) * channels;

    out.sample_rate = static_cast<std::uint32_t>(info->sampleRate);
    out.channels = channels;
    out.samples.assign(impl_->scratch.begin(),
                       impl_->scratch.begin() + static_cast<std::ptrdiff_t>(count));
    produced = frames > 0;
    return {};
}

} // namespace rtmp_server::transcoding::native
