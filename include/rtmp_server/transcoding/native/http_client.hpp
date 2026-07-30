#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::transcoding::native {

// A thin blocking HTTP(S) GET client over libcurl, for pulling an HLS source's
// playlists and segments. Deliberately minimal: one GET, a byte body, a total
// timeout. Runs only on a source-job worker thread, never on the media path.
class HttpClient {
public:
    explicit HttpClient(std::chrono::seconds timeout = std::chrono::seconds(15));
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Fetches `url` into `out` (replacing its contents). Fails on transport
    // error or any non-2xx status.
    [[nodiscard]] core::Result<void> get(const std::string& url, std::vector<std::byte>& out);

private:
    void* handle_ = nullptr; // CURL*
    std::chrono::seconds timeout_;
};

} // namespace rtmp_server::transcoding::native
