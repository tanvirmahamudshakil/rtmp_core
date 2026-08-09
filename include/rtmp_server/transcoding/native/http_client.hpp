#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::transcoding::native {

// A thin blocking HTTP(S) client over libcurl, for pulling an HLS source's
// playlists/segments (bounded get()/peek()) or a raw continuous HTTP-TS feed
// (unbounded stream()). Runs only on a source-job worker thread, never on the
// media path.
class HttpClient {
public:
    // Called with each response chunk as it arrives; return false to abort the
    // transfer early (e.g. the job is stopping).
    using ChunkHandler = std::function<bool(std::span<const std::byte> chunk)>;

    explicit HttpClient(std::chrono::seconds timeout = std::chrono::seconds(15));
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Fetches `url` into `out` (replacing its contents). Fails on transport
    // error or any non-2xx status. Intended for small, bounded bodies
    // (playlists, TS segments) — never a continuously-flowing stream.
    // When `effective_url` is provided it receives libcurl's final URL after
    // redirects. HLS callers must resolve relative playlist entries against
    // that URL, not the originally requested host.
    [[nodiscard]] core::Result<void> get(const std::string& url, std::vector<std::byte>& out,
                                         std::string* effective_url = nullptr);

    // Fetches at most `max_bytes` of `url`'s body (following redirects) into
    // `out`, then aborts the transfer — a bounded sniff of a source that may be
    // a bounded playlist or an unbounded live feed, without downloading the
    // latter in full. Succeeds once at least one byte was captured.
    [[nodiscard]] core::Result<void> peek(const std::string& url, std::size_t max_bytes,
                                          std::vector<std::byte>& out);

    // Performs one GET with no total time limit, invoking `on_chunk` as bytes
    // arrive and `should_continue` periodically so a caller can abort a
    // never-ending body (e.g. a raw continuous HTTP-TS live feed). Returns once
    // the connection closes, stalls, or either callback asks to stop.
    [[nodiscard]] core::Result<void> stream(const std::string& url,
                                            const std::function<bool()>& should_continue,
                                            const ChunkHandler& on_chunk);

private:
    void* handle_ = nullptr; // CURL*
    std::chrono::seconds timeout_;
};

} // namespace rtmp_server::transcoding::native
