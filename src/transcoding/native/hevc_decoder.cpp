#include "rtmp_server/transcoding/native/hevc_decoder.hpp"

#include <libde265/de265.h>

#include <chrono>
#include <cstring>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::transcoding::native {

namespace detail {

namespace {

// libde265's public API distinguishes two things that OpenH264 folds into one
// bitmask: de265_decode()'s return value (a real decode error, except the
// sentinel "feed me more data" code) and a separate queue of non-fatal
// warnings drained via de265_get_warning(). Only the former needs a
// fatal/non-fatal policy here.
constexpr int kOk = static_cast<int>(DE265_OK);
constexpr int kWaitingForInputData = static_cast<int>(DE265_ERROR_WAITING_FOR_INPUT_DATA);

} // namespace

bool libde265_decode_error_is_fatal(int error_code) noexcept {
    return error_code != kOk && error_code != kWaitingForInputData;
}

std::string libde265_decode_error_description(int error_code) {
    return de265_get_error_text(static_cast<de265_error>(error_code));
}

} // namespace detail

namespace {

using observability::LogLevel;

core::Error decode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

// Copies one libde265 output plane (which may be strided) into a tightly
// packed destination sized width x height. Mirrors h264_decoder.cpp's
// copy_plane so both decoders feed YuvFrame identically.
void copy_plane(std::uint8_t* dst, int dst_stride, const std::uint8_t* src, int src_stride,
                std::uint32_t width, std::uint32_t height) {
    const auto dst_pitch = static_cast<std::size_t>(dst_stride);
    const auto src_pitch = static_cast<std::size_t>(src_stride);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + row * dst_pitch, src + row * src_pitch, width);
    }
}

} // namespace

struct HevcDecoder::Impl {
    de265_decoder_context* decoder = nullptr;
    std::uint64_t suppressed_warning_logs = 0;
    std::chrono::steady_clock::time_point last_warning_log{};

    ~Impl() {
        if (decoder) {
            de265_free_decoder(decoder);
        }
    }
};

HevcDecoder::HevcDecoder() : impl_(std::make_unique<Impl>()) {}
HevcDecoder::~HevcDecoder() = default;

core::Result<void> HevcDecoder::initialize() {
    impl_->decoder = de265_new_decoder();
    if (impl_->decoder == nullptr) {
        return decode_error("failed to create libde265 decoder");
    }
    // Keep decode deterministic and confined to the calling thread -- source
    // jobs already fan renditions across a thread pool one level up
    // (SourceTranscoder::render_pool_); an internal libde265 thread pool
    // would just add contention without a latency benefit here.
    de265_start_worker_threads(impl_->decoder, 0);
    return {};
}

core::Result<void> HevcDecoder::decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                       YuvFrame& out, bool& produced) {
    produced = false;
    if (impl_->decoder == nullptr) return decode_error("decoder not initialized");

    const auto* data = reinterpret_cast<const std::uint8_t*>(annexb.data());
    const de265_error push_err =
        de265_push_data(impl_->decoder, data, static_cast<int>(annexb.size()),
                        static_cast<de265_PTS>(pts_90k), nullptr);
    if (detail::libde265_decode_error_is_fatal(static_cast<int>(push_err))) {
        return decode_error("libde265 failed to push data: " +
                            detail::libde265_decode_error_description(static_cast<int>(push_err)));
    }

    // Drain the decode loop until libde265 asks for more input. Annex B
    // access units can be parameter-set-only (no picture produced) just like
    // OpenH264's input, so "no error, nothing decoded yet" is expected.
    for (;;) {
        int more = 0;
        const de265_error err = de265_decode(impl_->decoder, &more);
        const auto err_value = static_cast<int>(err);
        if (err_value == static_cast<int>(DE265_ERROR_WAITING_FOR_INPUT_DATA)) break;
        if (detail::libde265_decode_error_is_fatal(err_value)) {
            return decode_error("libde265 fatal decode error: " +
                                detail::libde265_decode_error_description(err_value));
        }
        if (!more) break;
    }

    // Warnings (malformed-but-recoverable bitstream conditions -- lost
    // references, out-of-range parameters, etc.) are queued separately from
    // de265_decode's return value. Log them the same way h264_decoder.cpp
    // logs OpenH264's recoverable states: rate-limited, not fatal, so a live
    // source with occasional bitstream noise doesn't restart the whole job.
    for (;;) {
        const de265_error warning = de265_get_warning(impl_->decoder);
        if (warning == DE265_OK) break;
        const auto now = std::chrono::steady_clock::now();
        const bool log_due = impl_->last_warning_log == std::chrono::steady_clock::time_point{} ||
                             now - impl_->last_warning_log >= std::chrono::seconds(5);
        if (log_due) {
            RTMP_LOG(LogLevel::Warn, "hevc_decoder", "recoverable_decode_warning",
                     {{"warning", detail::libde265_decode_error_description(static_cast<int>(warning))},
                      {"suppressed_since_last_log", std::to_string(impl_->suppressed_warning_logs)}});
            impl_->last_warning_log = now;
            impl_->suppressed_warning_logs = 0;
        } else {
            ++impl_->suppressed_warning_logs;
        }
    }

    const de265_image* image = de265_get_next_picture(impl_->decoder);
    if (image == nullptr) return {}; // no complete picture ready yet

    if (de265_get_chroma_format(image) != de265_chroma_420) {
        de265_release_next_picture(impl_->decoder);
        return decode_error("libde265 produced a non-4:2:0 frame; only I420 is supported");
    }

    const auto width = static_cast<std::uint32_t>(de265_get_image_width(image, 0));
    const auto height = static_cast<std::uint32_t>(de265_get_image_height(image, 0));
    if (width < 2 || height < 2) {
        de265_release_next_picture(impl_->decoder);
        return decode_error("libde265 produced an invalid frame size");
    }

    int y_stride = 0, u_stride = 0, v_stride = 0;
    const std::uint8_t* y_plane = de265_get_image_plane(image, 0, &y_stride);
    const std::uint8_t* u_plane = de265_get_image_plane(image, 1, &u_stride);
    const std::uint8_t* v_plane = de265_get_image_plane(image, 2, &v_stride);
    if (y_plane == nullptr || u_plane == nullptr || v_plane == nullptr) {
        de265_release_next_picture(impl_->decoder);
        return decode_error("libde265 returned a null image plane");
    }

    out.allocate(width, height);
    out.pts_90k = static_cast<std::int64_t>(de265_get_image_PTS(image));
    const std::uint32_t chroma_w = (width + 1) / 2;
    const std::uint32_t chroma_h = (height + 1) / 2;
    copy_plane(out.y.data(), out.y_stride, y_plane, y_stride, width, height);
    copy_plane(out.u.data(), out.u_stride, u_plane, u_stride, chroma_w, chroma_h);
    copy_plane(out.v.data(), out.v_stride, v_plane, v_stride, chroma_w, chroma_h);

    de265_release_next_picture(impl_->decoder);
    produced = true;
    return {};
}

} // namespace rtmp_server::transcoding::native
