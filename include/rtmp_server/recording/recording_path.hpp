#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::recording {

// Safe construction of on-disk recording paths from client-controlled names
// (Phase 8 security task 10, "review directory traversal risks in recording
// and HLS paths").
//
// Threat model. An RTMP `publish` carries an application name and a stream
// name that both originate from the peer. The HLS side never touches the
// filesystem (hls::SegmentStore keeps segments in memory and serves them by
// exact map key, and control/hls_http_handler.cpp already rejects "." / ".."
// / backslash path components), so recording is the only place a
// peer-supplied name can become a path. Before Phase 8 no helper existed to
// build that path at all — recording::AsyncFileSink::open() took a
// fully-formed path from its caller, so the sanitisation obligation was
// unowned. This header owns it.
//
// Design choice: allow-list, not deny-list. Blacklisting "../" invites
// bypasses (".../...//", "..%2f", overlong UTF-8, NUL truncation, Windows
// "..\\", alternate data streams). Instead a component is accepted only if
// every byte is in [A-Za-z0-9._-] and the component is not "." or "..". That
// makes traversal, absolute paths, NUL injection and separator injection
// unrepresentable rather than merely filtered.

// Characters permitted in a sanitised path component.
[[nodiscard]] bool is_safe_path_char(char c) noexcept;

// True if `component` is safe to use verbatim as a single filesystem path
// component: non-empty, no more than kMaxComponentLength bytes, every byte
// in the allow-list, not the "." or ".." directory aliases, and not starting
// with '-' (argv injection into operator tooling) or '.' (hidden file that
// glob-based retention sweeps skip).
[[nodiscard]] bool is_safe_path_component(std::string_view component) noexcept;

// Upper bound on a single component. 100 keeps
// <application>/<stream>-<timestamp>.flv comfortably inside the 255-byte
// per-component limit ext4/XFS/APFS all impose, so a long-but-legal stream
// name fails our explicit check rather than a confusing ENAMETOOLONG.
inline constexpr std::size_t kMaxComponentLength = 100;

// Rewrites `component` into a safe one by replacing every disallowed byte
// with '_' and truncating to kMaxComponentLength. Returns an error only if
// the result would be empty or a directory alias — i.e. the caller must
// still handle rejection, this is not a "always succeeds" escape hatch.
//
// Prefer build_recording_path(); this is exposed for callers that need to
// derive a display/label string from the same rules.
[[nodiscard]] core::Result<std::string> sanitize_path_component(std::string_view component);

// Builds "<directory>/<application>/<stream_name>-<started_at_unix>.flv".
//
// `application` and `stream_name` are rejected outright (not silently
// rewritten) when unsafe: a recording written under a name that differs from
// the one the operator asked for is worse than no recording, because it
// breaks the mapping the management API and retention sweeper rely on.
// `directory` is operator-controlled configuration (ServerConfig::
// recording_directory), so it is used as given.
//
// Returns core::ErrorCode::InvalidArgument for an unsafe or empty component.
[[nodiscard]] core::Result<std::string> build_recording_path(std::string_view directory,
                                                             std::string_view application,
                                                             std::string_view stream_name,
                                                             std::int64_t started_at_unix);

} // namespace rtmp_server::recording
