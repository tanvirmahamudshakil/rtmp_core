#pragma once

#include <cstdint>
#include <string>

namespace rtmp_server::transcoding::native {

// H.264 level_idc for a rendition, from ISO/IEC 14496-10 Table A-1 (MaxFS in
// macroblocks and MaxMBPS in macroblocks/second). The lowest level that can
// carry the geometry is chosen, matching what x264 signals in the SPS it
// actually emits.
[[nodiscard]] std::uint32_t h264_level_idc(std::uint32_t width, std::uint32_t height,
                                           std::uint32_t fps);

// The RFC 6381 CODECS attribute for one H.264 + AAC rendition of a source
// job's ladder, as EXT-X-STREAM-INF requires it.
//
// This has to describe what the encoders will really produce. A master
// playlist that under-declares (the ladder used to hard-code
// "avc1.64001f,mp4a.40.2" for every rung, i.e. High profile at level 3.1 and
// AAC-LC) tells a player that a 1080p variant is a 720p-class stream, and
// tells it a 48 kbit HE-AACv2 rung is plain AAC-LC. RFC 8216 requires every
// format present in the media to be listed, and a player is entitled to
// pick — or reject — a variant on this string alone.
//
// `width`/`height` of 0 mean "keep the source resolution", which is not
// known when the master playlist is published; the level is then declared
// permissively rather than too low, since a decoder that accepts a higher
// level accepts everything under it, whereas the reverse is a real decode
// failure.
//
// The audio object type mirrors build_aac_param_set's own choice for this
// bitrate, assuming a stereo source (the ladder's audio channel count is
// likewise unknown until the source's first frame decodes). For a mono
// source the encoder produces HE-AAC where this says HE-AACv2; both are SBR
// AAC and every decoder that handles one handles the other.
[[nodiscard]] std::string hls_codecs_attribute(std::uint32_t width, std::uint32_t height,
                                               std::uint32_t fps, std::uint32_t audio_bitrate);

} // namespace rtmp_server::transcoding::native
