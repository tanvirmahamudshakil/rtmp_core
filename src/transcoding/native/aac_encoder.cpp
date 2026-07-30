#include "rtmp_server/transcoding/native/aac_encoder.hpp"

#include <fdk-aac/aacenc_lib.h>

#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error encode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

} // namespace

struct AacEncoder::Impl {
    HANDLE_AACENCODER handle = nullptr;
    std::vector<std::uint8_t> out_buf = std::vector<std::uint8_t>(8192);

    ~Impl() {
        if (handle) aacEncClose(&handle);
    }
};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() = default;

core::Result<void> AacEncoder::open(const AacParamSet& params) {
    channels_ = params.channels == 0 ? 2 : params.channels;
    sample_rate_ = params.sample_rate == 0 ? 44100 : params.sample_rate;

    // Try the requested audio-object type first; if this libfdk build cannot
    // configure it (some builds lack HE-AAC / HE-AACv2), fall back to AAC-LC
    // rather than failing the whole rendition.
    const int requested = params.audio_object_type();
    if (auto r = open_with_aot(params, requested); r) return {};
    if (requested == 2) return open_with_aot(params, 2); // already LC: surface its error
    return open_with_aot(params, 2);
}

core::Result<void> AacEncoder::open_with_aot(const AacParamSet& params, int aot) {
    if (impl_->handle != nullptr) {
        aacEncClose(&impl_->handle);
        impl_->handle = nullptr;
    }
    if (aacEncOpen(&impl_->handle, 0, channels_) != AACENC_OK || impl_->handle == nullptr) {
        return encode_error("aacEncOpen failed");
    }
    auto set = [&](AACENC_PARAM param, UINT value, const char* name) -> core::Result<void> {
        if (aacEncoder_SetParam(impl_->handle, param, value) != AACENC_OK) {
            return encode_error(std::string("aacEncoder_SetParam failed for ") + name);
        }
        return {};
    };
    if (auto r = set(AACENC_AOT, static_cast<UINT>(aot), "AOT"); !r) return r;
    if (auto r = set(AACENC_SAMPLERATE, sample_rate_, "SAMPLERATE"); !r) return r;
    if (auto r = set(AACENC_CHANNELMODE, channels_ == 1 ? MODE_1 : MODE_2, "CHANNELMODE"); !r)
        return r;
    if (auto r = set(AACENC_BITRATE, params.bitrate, "BITRATE"); !r) return r;
    // TT_MP4_ADTS: the encoder frames each access unit with an ADTS header, so
    // output feeds the TS muxer's audio path directly.
    if (auto r = set(AACENC_TRANSMUX, TT_MP4_ADTS, "TRANSMUX"); !r) return r;
    if (auto r = set(AACENC_AFTERBURNER, params.afterburner ? 1U : 0U, "AFTERBURNER"); !r) return r;

    if (aacEncEncode(impl_->handle, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        return encode_error("aacEncEncode initialization failed");
    }
    AACENC_InfoStruct info{};
    if (aacEncInfo(impl_->handle, &info) != AACENC_OK) {
        return encode_error("aacEncInfo failed");
    }
    frame_length_ = static_cast<std::uint32_t>(info.frameLength);
    if (info.maxOutBufBytes > impl_->out_buf.size()) impl_->out_buf.resize(info.maxOutBufBytes);
    return {};
}

core::Result<bool> AacEncoder::encode_one(const std::int16_t* interleaved, int in_samples,
                                          EncodedAudioFrame& frame) {
    void* in_ptr = const_cast<std::int16_t*>(interleaved);
    int in_identifier = IN_AUDIO_DATA;
    int in_elem_size = sizeof(std::int16_t);
    int in_size = in_samples * static_cast<int>(sizeof(std::int16_t));

    void* out_ptr = impl_->out_buf.data();
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_elem_size = 1;
    int out_size = static_cast<int>(impl_->out_buf.size());

    AACENC_BufDesc in_desc{};
    in_desc.numBufs = 1;
    in_desc.bufs = &in_ptr;
    in_desc.bufferIdentifiers = &in_identifier;
    in_desc.bufSizes = &in_size;
    in_desc.bufElSizes = &in_elem_size;

    AACENC_BufDesc out_desc{};
    out_desc.numBufs = 1;
    out_desc.bufs = &out_ptr;
    out_desc.bufferIdentifiers = &out_identifier;
    out_desc.bufSizes = &out_size;
    out_desc.bufElSizes = &out_elem_size;

    AACENC_InArgs in_args{};
    in_args.numInSamples = in_samples; // -1 signals end of stream (flush)
    AACENC_OutArgs out_args{};

    const AACENC_ERROR err = aacEncEncode(impl_->handle, &in_desc, &out_desc, &in_args, &out_args);
    if (err == AACENC_ENCODE_EOF) return false; // no more output while flushing
    if (err != AACENC_OK) return encode_error("aacEncEncode failed");
    if (out_args.numOutBytes <= 0) return false; // encoder still priming

    frame.adts.resize(static_cast<std::size_t>(out_args.numOutBytes));
    std::memcpy(frame.adts.data(), impl_->out_buf.data(),
                static_cast<std::size_t>(out_args.numOutBytes));
    frame.samples_per_channel = frame_length_;
    return true;
}

core::Result<void> AacEncoder::drain(std::vector<EncodedAudioFrame>& out, bool flushing) {
    const std::size_t frame_samples = static_cast<std::size_t>(frame_length_) * channels_;
    std::size_t consumed = 0;
    while (buffer_.size() - consumed >= frame_samples) {
        EncodedAudioFrame frame;
        auto produced =
            encode_one(buffer_.data() + consumed, static_cast<int>(frame_samples), frame);
        if (!produced) return produced.error();
        if (produced.value()) out.push_back(std::move(frame));
        consumed += frame_samples;
    }
    if (consumed > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));

    if (flushing) {
        // Zero-pad any trailing partial frame, then drain the encoder's delay.
        if (!buffer_.empty()) {
            buffer_.resize(frame_samples, 0);
            EncodedAudioFrame frame;
            if (auto produced = encode_one(buffer_.data(), static_cast<int>(frame_samples), frame);
                !produced) {
                return produced.error();
            } else if (produced.value()) {
                out.push_back(std::move(frame));
            }
            buffer_.clear();
        }
        for (;;) {
            EncodedAudioFrame frame;
            auto produced = encode_one(nullptr, -1, frame);
            if (!produced) return produced.error();
            if (!produced.value()) break;
            out.push_back(std::move(frame));
        }
    }
    return {};
}

core::Result<void> AacEncoder::encode(const PcmBlock& pcm, std::vector<EncodedAudioFrame>& out) {
    if (impl_->handle == nullptr) return encode_error("encoder not opened");
    if (pcm.channels != channels_) return encode_error("PCM channel count changed mid-stream");
    buffer_.insert(buffer_.end(), pcm.samples.begin(), pcm.samples.end());
    return drain(out, false);
}

core::Result<void> AacEncoder::flush(std::vector<EncodedAudioFrame>& out) {
    if (impl_->handle == nullptr) return {};
    return drain(out, true);
}

} // namespace rtmp_server::transcoding::native
