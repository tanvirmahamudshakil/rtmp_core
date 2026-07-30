#include "rtmp_server/transcoding/native/http_client.hpp"

#include <curl/curl.h>

#include <mutex>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error http_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Network, std::move(message));
}

// libcurl global init is process-wide and must happen once before any easy
// handle is created; guard it with a call_once.
void ensure_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<std::byte>*>(userdata);
    const std::size_t bytes = size * nmemb;
    const auto* begin = reinterpret_cast<const std::byte*>(ptr);
    out->insert(out->end(), begin, begin + bytes);
    return bytes;
}

} // namespace

HttpClient::HttpClient(std::chrono::seconds timeout) : timeout_(timeout) {
    ensure_global_init();
    handle_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (handle_) curl_easy_cleanup(static_cast<CURL*>(handle_));
}

core::Result<void> HttpClient::get(const std::string& url, std::vector<std::byte>& out) {
    if (handle_ == nullptr) return http_error("curl handle not initialized");
    out.clear();
    auto* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // allow gzip
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rtmp_core-source-transcoder/1.0");

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) return http_error(std::string("HTTP GET failed: ") + curl_easy_strerror(code));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        return http_error("HTTP GET returned status " + std::to_string(status) + " for " + url);
    }
    return {};
}

} // namespace rtmp_server::transcoding::native
