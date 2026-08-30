#include "rtmp_server/transcoding/native/http_client.hpp"

#include <curl/curl.h>

#include <algorithm>
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

struct PeekState {
    std::vector<std::byte>* out;
    std::size_t max_bytes;
};

// Collects up to max_bytes, then returns a short count to make curl abort the
// transfer with CURLE_WRITE_ERROR — the intended way to stop early.
std::size_t peek_write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* state = static_cast<PeekState*>(userdata);
    const std::size_t bytes = size * nmemb;
    const auto* begin = reinterpret_cast<const std::byte*>(ptr);
    if (state->out->size() >= state->max_bytes) return 0;
    const std::size_t take = std::min(bytes, state->max_bytes - state->out->size());
    state->out->insert(state->out->end(), begin, begin + take);
    return state->out->size() >= state->max_bytes ? 0 : bytes;
}

struct StreamState {
    const HttpClient::ChunkHandler* on_chunk;
    const std::function<bool()>* should_continue;
};

std::size_t stream_write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* state = static_cast<StreamState*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (state->should_continue && !(*state->should_continue)()) return 0;
    const auto* begin = reinterpret_cast<const std::byte*>(ptr);
    if (!(*state->on_chunk)(std::span<const std::byte>(begin, bytes))) return 0;
    return bytes;
}

int stream_progress_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<StreamState*>(userdata);
    return (state->should_continue && !(*state->should_continue)()) ? 1 : 0;
}

} // namespace

HttpClient::HttpClient(std::chrono::seconds timeout) : timeout_(timeout) {
    ensure_global_init();
    handle_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (handle_) curl_easy_cleanup(static_cast<CURL*>(handle_));
}

core::Result<void> HttpClient::get(const std::string& url, std::vector<std::byte>& out,
                                   std::string* effective_url) {
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

    if (forbid_reuse_) {
        // Do not reuse a pooled connection and do not leave this one pooled:
        // a panel that caps concurrent connections per account counts an idle
        // keep-alive from a previous request against the next one.
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    }
    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) return http_error(std::string("HTTP GET failed: ") + curl_easy_strerror(code));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        return http_error("HTTP GET returned status " + std::to_string(status) + " for " + url);
    }
    if (effective_url != nullptr) {
        char* resolved = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &resolved);
        *effective_url = resolved != nullptr ? resolved : url;
    }
    return {};
}

core::Result<void> HttpClient::peek(const std::string& url, std::size_t max_bytes,
                                    std::vector<std::byte>& out) {
    if (handle_ == nullptr) return http_error("curl handle not initialized");
    out.clear();
    auto* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);
    PeekState state{&out, max_bytes};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, peek_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rtmp_core-source-transcoder/1.0");

    if (forbid_reuse_) {
        // Do not reuse a pooled connection and do not leave this one pooled:
        // a panel that caps concurrent connections per account counts an idle
        // keep-alive from a previous request against the next one.
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    }
    const CURLcode code = curl_easy_perform(curl);
    // A short write from peek_write_cb (i.e. we already have max_bytes) is the
    // expected way this transfer ends; only a genuine transport failure with
    // nothing captured is an error.
    if (code != CURLE_OK && code != CURLE_WRITE_ERROR) {
        return http_error(std::string("HTTP GET failed: ") + curl_easy_strerror(code));
    }
    if (out.empty()) return http_error("empty response from " + url);
    return {};
}

core::Result<void> HttpClient::stream(const std::string& url,
                                      const std::function<bool()>& should_continue,
                                      const ChunkHandler& on_chunk) {
    if (handle_ == nullptr) return http_error("curl handle not initialized");
    auto* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);
    StreamState state{&on_chunk, &should_continue};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // a live feed has no natural end
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // Abort a connection that has effectively stalled, so a dead upstream
    // doesn't pin this worker thread forever.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rtmp_core-source-transcoder/1.0");
    // Poll should_continue even while no bytes are arriving, so stop() is
    // responsive on an idle-but-open connection.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

    if (forbid_reuse_) {
        // Do not reuse a pooled connection and do not leave this one pooled:
        // a panel that caps concurrent connections per account counts an idle
        // keep-alive from a previous request against the next one.
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    }
    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_ABORTED_BY_CALLBACK || code == CURLE_WRITE_ERROR) return {}; // caller asked to stop
    if (code != CURLE_OK) return http_error(std::string("HTTP GET failed: ") + curl_easy_strerror(code));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        return http_error("HTTP GET returned status " + std::to_string(status) + " for " + url);
    }
    return {};
}

} // namespace rtmp_server::transcoding::native
