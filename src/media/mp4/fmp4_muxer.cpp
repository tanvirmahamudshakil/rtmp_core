#include "rtmp_server/media/mp4/fmp4_muxer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace rtmp_server::media::mp4 {

namespace {

core::Error invalid(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

void put_u8(std::vector<std::byte>& out, std::uint8_t value) { out.push_back(std::byte{value}); }

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_u24(std::vector<std::byte>& out, std::uint32_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 16));
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(value >> 16));
    put_u16(out, static_cast<std::uint16_t>(value));
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value >> 32));
    put_u32(out, static_cast<std::uint32_t>(value));
}

void put_fourcc(std::vector<std::byte>& out, const char (&code)[5]) {
    for (int i = 0; i < 4; ++i) put_u8(out, static_cast<std::uint8_t>(code[i]));
}

void put_bytes(std::vector<std::byte>& out, std::span<const std::byte> data) {
    out.insert(out.end(), data.begin(), data.end());
}

void put_zeros(std::vector<std::byte>& out, std::size_t count) {
    out.insert(out.end(), count, std::byte{0});
}

void patch_u32(std::vector<std::byte>& out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::byte>(value >> 24);
    out[offset + 1] = static_cast<std::byte>(value >> 16);
    out[offset + 2] = static_cast<std::byte>(value >> 8);
    out[offset + 3] = static_cast<std::byte>(value);
}

// Opens a box, returning the offset of its (still zero) size field. Every box
// is closed with close_box(), which back-patches the real size — the only way
// to write nested boxes in one pass without pre-computing every size.
[[nodiscard]] std::size_t open_box(std::vector<std::byte>& out, const char (&type)[5]) {
    const std::size_t start = out.size();
    put_u32(out, 0);
    put_fourcc(out, type);
    return start;
}

void close_box(std::vector<std::byte>& out, std::size_t start) {
    patch_u32(out, start, static_cast<std::uint32_t>(out.size() - start));
}

[[nodiscard]] std::size_t open_full_box(std::vector<std::byte>& out, const char (&type)[5],
                                        std::uint8_t version, std::uint32_t flags) {
    const std::size_t start = open_box(out, type);
    put_u8(out, version);
    put_u24(out, flags);
    return start;
}

// The identity 3x3 transform every track matrix here uses (16.16 / 2.30 fixed
// point, ISO/IEC 14496-12 6.2.2).
void put_identity_matrix(std::vector<std::byte>& out) {
    put_u32(out, 0x00010000);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0x00010000);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0x40000000);
}

void write_hdlr(std::vector<std::byte>& out, const char (&handler)[5], const char* name) {
    const std::size_t box = open_full_box(out, "hdlr", 0, 0);
    put_u32(out, 0); // pre_defined
    put_fourcc(out, handler);
    put_zeros(out, 12); // reserved
    for (const char* c = name; *c != '\0'; ++c) put_u8(out, static_cast<std::uint8_t>(*c));
    put_u8(out, 0); // null-terminated name
    close_box(out, box);
}

void write_dinf(std::vector<std::byte>& out) {
    const std::size_t dinf = open_box(out, "dinf");
    const std::size_t dref = open_full_box(out, "dref", 0, 0);
    put_u32(out, 1); // entry_count
    // A self-contained `url ` entry (flags bit 0) means "the media is in this
    // file", which is true of every fragment this muxer emits.
    const std::size_t url = open_full_box(out, "url ", 0, 1);
    close_box(out, url);
    close_box(out, dref);
    close_box(out, dinf);
}

// MPEG-4 descriptor length in the minimal single-byte form. Every descriptor
// written here is far below 128 bytes (an AudioSpecificConfig is 2-5), so the
// extended 0x80-continuation form would buy nothing.
void put_descriptor_header(std::vector<std::byte>& out, std::uint8_t tag, std::size_t length) {
    put_u8(out, tag);
    put_u8(out, static_cast<std::uint8_t>(length & 0x7F));
}

void write_esds(std::vector<std::byte>& out, std::span<const std::byte> audio_specific_config) {
    const std::size_t esds = open_full_box(out, "esds", 0, 0);

    const std::size_t dsi_length = audio_specific_config.size();
    const std::size_t dcd_length = 13 + 2 + dsi_length; // config fields + DSI header + DSI
    const std::size_t es_length = 3 + 2 + dcd_length + 2 + 1;

    put_descriptor_header(out, 0x03, es_length); // ES_Descriptor
    put_u16(out, 0);                             // ES_ID (0 is legal inside an MP4 track)
    put_u8(out, 0);                              // no stream dependency, no URL, no OCR

    put_descriptor_header(out, 0x04, dcd_length); // DecoderConfigDescriptor
    put_u8(out, 0x40);                            // objectTypeIndication: MPEG-4 Audio
    put_u8(out, 0x15);                            // streamType 5 (audio), upstream 0, reserved 1
    put_u24(out, 0);                              // bufferSizeDB
    put_u32(out, 0);                              // maxBitrate (unknown for live passthrough)
    put_u32(out, 0);                              // avgBitrate

    put_descriptor_header(out, 0x05, dsi_length); // DecoderSpecificInfo
    put_bytes(out, audio_specific_config);

    put_descriptor_header(out, 0x06, 1); // SLConfigDescriptor
    put_u8(out, 0x02);                   // predefined: MP4 file

    close_box(out, esds);
}

void write_video_sample_entry(std::vector<std::byte>& out, VideoCodec codec,
                              std::span<const std::byte> decoder_config,
                              const VideoDimensions& dimensions) {
    const std::size_t entry =
        (codec == VideoCodec::Hevc) ? open_box(out, "hvc1") : open_box(out, "avc1");
    put_zeros(out, 6);  // reserved
    put_u16(out, 1);    // data_reference_index
    put_u16(out, 0);    // pre_defined
    put_u16(out, 0);    // reserved
    put_zeros(out, 12); // pre_defined[3]
    put_u16(out, static_cast<std::uint16_t>(dimensions.width));
    put_u16(out, static_cast<std::uint16_t>(dimensions.height));
    put_u32(out, 0x00480000); // horizresolution, 72 dpi
    put_u32(out, 0x00480000); // vertresolution, 72 dpi
    put_u32(out, 0);          // reserved
    put_u16(out, 1);          // frame_count
    put_zeros(out, 32);       // compressorname
    put_u16(out, 0x0018);     // depth
    put_u16(out, 0xFFFF);     // pre_defined = -1

    const std::size_t config_box =
        (codec == VideoCodec::Hevc) ? open_box(out, "hvcC") : open_box(out, "avcC");
    put_bytes(out, decoder_config);
    close_box(out, config_box);

    close_box(out, entry);
}

void write_audio_sample_entry(std::vector<std::byte>& out, const aac::AudioSpecificConfig& config,
                              std::span<const std::byte> audio_specific_config) {
    const std::size_t entry = open_box(out, "mp4a");
    put_zeros(out, 6); // reserved
    put_u16(out, 1);   // data_reference_index
    put_zeros(out, 8); // reserved
    put_u16(out, config.channel_configuration);
    put_u16(out, 16); // samplesize
    put_u16(out, 0);  // pre_defined
    put_u16(out, 0);  // reserved
    // 16.16 fixed point cannot represent a rate above 65535, so HE-AAC's
    // doubled rate is written as its core rate here — what every muxer does;
    // the authoritative rate stays in the esds AudioSpecificConfig.
    const std::uint32_t rate = config.sample_rate();
    put_u32(out, (rate > 0xFFFFu ? 0xFFFFu : rate) << 16);
    write_esds(out, audio_specific_config);
    close_box(out, entry);
}

// A fragmented file carries no samples in `moov`, but the sample-table boxes
// must still be present and well-formed.
void write_empty_sample_tables(std::vector<std::byte>& out) {
    const std::size_t stts = open_full_box(out, "stts", 0, 0);
    put_u32(out, 0);
    close_box(out, stts);
    const std::size_t stsc = open_full_box(out, "stsc", 0, 0);
    put_u32(out, 0);
    close_box(out, stsc);
    const std::size_t stsz = open_full_box(out, "stsz", 0, 0);
    put_u32(out, 0); // sample_size
    put_u32(out, 0); // sample_count
    close_box(out, stsz);
    const std::size_t stco = open_full_box(out, "stco", 0, 0);
    put_u32(out, 0);
    close_box(out, stco);
}

void write_tkhd(std::vector<std::byte>& out, std::uint32_t track_id, bool is_audio,
                const VideoDimensions& dimensions) {
    // flags 7 = track_enabled | track_in_movie | track_in_preview
    const std::size_t tkhd = open_full_box(out, "tkhd", 0, 7);
    put_u32(out, 0); // creation_time
    put_u32(out, 0); // modification_time
    put_u32(out, track_id);
    put_u32(out, 0); // reserved
    put_u32(out, 0); // duration: unknown, this is live
    put_zeros(out, 8);
    put_u16(out, 0);                     // layer
    put_u16(out, 0);                     // alternate_group
    put_u16(out, is_audio ? 0x0100 : 0); // volume
    put_u16(out, 0);                     // reserved
    put_identity_matrix(out);
    put_u32(out, is_audio ? 0u : (dimensions.width << 16));
    put_u32(out, is_audio ? 0u : (dimensions.height << 16));
    close_box(out, tkhd);
}

void write_trex(std::vector<std::byte>& out, std::uint32_t track_id) {
    const std::size_t trex = open_full_box(out, "trex", 0, 0);
    put_u32(out, track_id);
    put_u32(out, 1); // default_sample_description_index
    put_u32(out, 0); // default_sample_duration
    put_u32(out, 0); // default_sample_size
    put_u32(out, 0); // default_sample_flags
    close_box(out, trex);
}

// `trun` sample_flags (ISO/IEC 14496-12 8.8.3.1). A sync sample depends on
// nothing and is not marked non-sync; every other sample is the inverse.
// These two values are what let a player's join or seek land on a keyframe.
constexpr std::uint32_t kSampleFlagsSync = 0x02000000;
constexpr std::uint32_t kSampleFlagsNonSync = 0x01010000;

std::uint64_t total_sample_bytes(const Fmp4TrackFragment& fragment) {
    std::uint64_t total = 0;
    for (const auto& sample : fragment.samples) total += sample.data.size();
    return total;
}

// Writes one `traf`, leaving the `trun` data_offset zeroed and reporting its
// position so write_fragment can back-patch it once the moof size is known.
void write_traf(std::vector<std::byte>& out, std::size_t moof_start, std::uint32_t track_id,
                const Fmp4TrackFragment& fragment, std::size_t& data_offset_position) {
    const std::size_t traf = open_box(out, "traf");

    // tfhd flags 0x020000 = default-base-is-moof: every trun data_offset is
    // relative to this moof, the only self-contained choice for a fragment
    // served as its own HTTP resource (a CMAF chunk / LL-HLS partial segment).
    const std::size_t tfhd = open_full_box(out, "tfhd", 0, 0x020000);
    put_u32(out, track_id);
    close_box(out, tfhd);

    const std::size_t tfdt = open_full_box(out, "tfdt", 1, 0);
    put_u64(out, fragment.base_decode_time);
    close_box(out, tfdt);

    // version 1: composition offsets are signed, which B-frames require.
    // flags: data-offset | sample-duration | sample-size | sample-flags |
    //        sample-composition-time-offset
    const std::size_t trun = open_full_box(out, "trun", 1, 0x000F01);
    put_u32(out, static_cast<std::uint32_t>(fragment.samples.size()));
    data_offset_position = out.size() - moof_start;
    put_u32(out, 0); // data_offset, patched by the caller
    for (const auto& sample : fragment.samples) {
        put_u32(out, sample.duration);
        put_u32(out, static_cast<std::uint32_t>(sample.data.size()));
        put_u32(out, sample.keyframe ? kSampleFlagsSync : kSampleFlagsNonSync);
        put_u32(out, static_cast<std::uint32_t>(sample.composition_offset));
    }
    close_box(out, trun);

    close_box(out, traf);
}

std::string hex_byte(std::uint8_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    return std::string{kHex[value >> 4], kHex[value & 0x0F]};
}

} // namespace

core::Result<std::vector<std::byte>> Fmp4Muxer::init_segment(const Fmp4InitConfig& config) const {
    if (!config.has_video() && !config.has_audio) {
        return invalid("fMP4 init segment needs at least one of a video or an audio track");
    }
    if (config.has_video() && config.video_decoder_config.empty()) {
        return invalid("fMP4 video track declared without an avcC/hvcC decoder configuration");
    }
    if (config.has_audio && config.audio_specific_config.empty()) {
        return invalid("fMP4 audio track declared without an AudioSpecificConfig");
    }
    if (config.has_video() && !config.video_dimensions.valid()) {
        return invalid("fMP4 video track declared without usable picture dimensions");
    }

    std::vector<std::byte> out;
    out.reserve(1024 + config.video_decoder_config.size());

    const std::size_t ftyp = open_box(out, "ftyp");
    put_fourcc(out, "iso6"); // major_brand
    put_u32(out, 0);         // minor_version
    put_fourcc(out, "iso6");
    put_fourcc(out, "mp41");
    put_fourcc(out, "cmfc"); // CMAF track profile
    close_box(out, ftyp);

    const std::size_t moov = open_box(out, "moov");

    const std::size_t mvhd = open_full_box(out, "mvhd", 0, 0);
    put_u32(out, 0); // creation_time
    put_u32(out, 0); // modification_time
    put_u32(out, kMovieTimescale);
    put_u32(out, 0);          // duration: unknown, live
    put_u32(out, 0x00010000); // rate 1.0
    put_u16(out, 0x0100);     // volume 1.0
    put_u16(out, 0);          // reserved
    put_zeros(out, 8);        // reserved
    put_identity_matrix(out);
    put_zeros(out, 24); // pre_defined[6]
    put_u32(out, kAudioTrackId + 1);
    close_box(out, mvhd);

    if (config.has_video()) {
        const std::size_t trak = open_box(out, "trak");
        write_tkhd(out, kVideoTrackId, false, config.video_dimensions);
        const std::size_t mdia = open_box(out, "mdia");
        const std::size_t mdhd = open_full_box(out, "mdhd", 0, 0);
        put_u32(out, 0); // creation_time
        put_u32(out, 0); // modification_time
        put_u32(out, kVideoTimescale);
        put_u32(out, 0);      // duration
        put_u16(out, 0x55C4); // language "und"
        put_u16(out, 0);      // pre_defined
        close_box(out, mdhd);
        write_hdlr(out, "vide", "VideoHandler");
        const std::size_t minf = open_box(out, "minf");
        const std::size_t vmhd = open_full_box(out, "vmhd", 0, 1);
        put_u16(out, 0);   // graphicsmode
        put_zeros(out, 6); // opcolor
        close_box(out, vmhd);
        write_dinf(out);
        const std::size_t stbl = open_box(out, "stbl");
        const std::size_t stsd = open_full_box(out, "stsd", 0, 0);
        put_u32(out, 1); // entry_count
        write_video_sample_entry(out, config.video_codec, config.video_decoder_config,
                                 config.video_dimensions);
        close_box(out, stsd);
        write_empty_sample_tables(out);
        close_box(out, stbl);
        close_box(out, minf);
        close_box(out, mdia);
        close_box(out, trak);
    }

    if (config.has_audio) {
        const std::uint32_t timescale = config.audio_config.sample_rate();
        if (timescale == 0) return invalid("AudioSpecificConfig carries a reserved sampling index");

        const std::size_t trak = open_box(out, "trak");
        write_tkhd(out, kAudioTrackId, true, {});
        const std::size_t mdia = open_box(out, "mdia");
        const std::size_t mdhd = open_full_box(out, "mdhd", 0, 0);
        put_u32(out, 0);
        put_u32(out, 0);
        put_u32(out, timescale);
        put_u32(out, 0);      // duration
        put_u16(out, 0x55C4); // "und"
        put_u16(out, 0);
        close_box(out, mdhd);
        write_hdlr(out, "soun", "SoundHandler");
        const std::size_t minf = open_box(out, "minf");
        const std::size_t smhd = open_full_box(out, "smhd", 0, 0);
        put_u16(out, 0); // balance
        put_u16(out, 0); // reserved
        close_box(out, smhd);
        write_dinf(out);
        const std::size_t stbl = open_box(out, "stbl");
        const std::size_t stsd = open_full_box(out, "stsd", 0, 0);
        put_u32(out, 1); // entry_count
        write_audio_sample_entry(out, config.audio_config, config.audio_specific_config);
        close_box(out, stsd);
        write_empty_sample_tables(out);
        close_box(out, stbl);
        close_box(out, minf);
        close_box(out, mdia);
        close_box(out, trak);
    }

    const std::size_t mvex = open_box(out, "mvex");
    if (config.has_video()) write_trex(out, kVideoTrackId);
    if (config.has_audio) write_trex(out, kAudioTrackId);
    close_box(out, mvex);

    close_box(out, moov);
    return out;
}

core::Result<void> Fmp4Muxer::write_fragment(std::vector<std::byte>& out,
                                             const Fmp4TrackFragment& video,
                                             const Fmp4TrackFragment& audio,
                                             std::uint32_t audio_timescale) {
    // Timescales are fixed by the init segment; the parameter documents the
    // caller's own bookkeeping and keeps the call site honest about which
    // clock its audio durations are in.
    (void)audio_timescale;
    if (video.empty() && audio.empty()) return invalid("fMP4 fragment carries no samples");

    const std::uint64_t video_bytes = total_sample_bytes(video);
    const std::uint64_t audio_bytes = total_sample_bytes(audio);
    const std::uint64_t media_bytes = video_bytes + audio_bytes;
    // A 32-bit `mdat` header cannot describe more than 4 GiB, and no live
    // fragment comes near it — refuse rather than silently truncate.
    if (media_bytes + 8 > std::numeric_limits<std::uint32_t>::max()) {
        return invalid("fMP4 fragment exceeds the 32-bit mdat size limit");
    }

    ++sequence_number_;

    std::vector<std::byte> moof;
    moof.reserve(256 + 16 * (video.samples.size() + audio.samples.size()));
    const std::size_t moof_start = open_box(moof, "moof");
    const std::size_t mfhd = open_full_box(moof, "mfhd", 0, 0);
    put_u32(moof, sequence_number_);
    close_box(moof, mfhd);

    std::size_t video_offset_position = 0;
    std::size_t audio_offset_position = 0;
    if (!video.empty()) write_traf(moof, moof_start, kVideoTrackId, video, video_offset_position);
    if (!audio.empty()) write_traf(moof, moof_start, kAudioTrackId, audio, audio_offset_position);
    close_box(moof, moof_start);

    // Media bytes follow the moof, video first: each track's data_offset is
    // the moof size plus the mdat header plus whatever precedes it.
    const auto moof_size = static_cast<std::uint32_t>(moof.size());
    if (!video.empty()) {
        patch_u32(moof, video_offset_position, moof_size + 8);
    }
    if (!audio.empty()) {
        patch_u32(moof, audio_offset_position,
                  moof_size + 8 + static_cast<std::uint32_t>(video_bytes));
    }

    out.insert(out.end(), moof.begin(), moof.end());

    put_u32(out, static_cast<std::uint32_t>(media_bytes + 8));
    put_fourcc(out, "mdat");
    for (const auto& sample : video.samples) put_bytes(out, sample.data);
    for (const auto& sample : audio.samples) put_bytes(out, sample.data);

    return {};
}

void Fmp4Muxer::write_styp(std::vector<std::byte>& out) {
    const std::size_t styp = open_box(out, "styp");
    put_fourcc(out, "msdh");
    put_u32(out, 0);
    put_fourcc(out, "msdh");
    put_fourcc(out, "msix");
    put_fourcc(out, "cmfs");
    close_box(out, styp);
}

std::string video_codec_string(VideoCodec codec, std::span<const std::byte> decoder_config) {
    if (codec == VideoCodec::H264) {
        // avc1.PPCCLL — profile_idc, profile_compatibility, level_idc, taken
        // straight out of the AVCDecoderConfigurationRecord (RFC 6381 3.3).
        if (decoder_config.size() < 4) return {};
        return "avc1." + hex_byte(static_cast<std::uint8_t>(decoder_config[1])) +
               hex_byte(static_cast<std::uint8_t>(decoder_config[2])) +
               hex_byte(static_cast<std::uint8_t>(decoder_config[3]));
    }
    if (codec == VideoCodec::Hevc) {
        // hvc1.<space><profile>.<compatibility flags, bit-reversed>.<tier>
        // <level>.<constraint bytes> — ISO/IEC 14496-15 Annex E.3.
        if (decoder_config.size() < 13) return {};
        const auto byte1 = static_cast<std::uint8_t>(decoder_config[1]);
        const std::uint8_t profile_space = (byte1 >> 6) & 0x03;
        const bool high_tier = ((byte1 >> 5) & 0x01) != 0;
        const std::uint8_t profile_idc = byte1 & 0x1F;

        std::uint32_t compatibility = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            compatibility = (compatibility << 8) | static_cast<std::uint8_t>(decoder_config[2 + i]);
        }
        std::uint32_t reversed = 0;
        for (int bit = 0; bit < 32; ++bit) {
            reversed = (reversed << 1) | ((compatibility >> bit) & 1u);
        }

        std::string result = "hvc1.";
        if (profile_space > 0) result += static_cast<char>('A' + profile_space - 1);
        result += std::to_string(profile_idc);
        result += '.';
        // Lower-case hex with leading zeros suppressed, per the Annex E form.
        std::string compat_hex;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const auto nibble = static_cast<std::uint8_t>((reversed >> shift) & 0x0F);
            if (compat_hex.empty() && nibble == 0 && shift != 0) continue;
            compat_hex += "0123456789abcdef"[nibble];
        }
        result += compat_hex;
        result += '.';
        result += high_tier ? 'H' : 'L';
        result += std::to_string(static_cast<unsigned>(static_cast<std::uint8_t>(decoder_config[12])));

        // The six constraint bytes follow, most significant first, with
        // trailing zero bytes omitted.
        std::size_t last_significant = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            if (static_cast<std::uint8_t>(decoder_config[6 + i]) != 0) last_significant = i + 1;
        }
        for (std::size_t i = 0; i < last_significant; ++i) {
            const std::string hex = hex_byte(static_cast<std::uint8_t>(decoder_config[6 + i]));
            result += '.';
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(hex[0])));
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(hex[1])));
        }
        return result;
    }
    return {};
}

std::string audio_codec_string(const aac::AudioSpecificConfig& config) {
    return "mp4a.40." + std::to_string(static_cast<unsigned>(config.object_type));
}

} // namespace rtmp_server::media::mp4
