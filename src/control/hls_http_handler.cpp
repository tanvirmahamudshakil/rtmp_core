#include "rtmp_server/control/hls_http_handler.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "rtmp_server/management/token.hpp"

namespace rtmp_server::control {

namespace {

std::unordered_map<std::string, std::string> parse_query(std::string_view query) {
    std::unordered_map<std::string, std::string> params;
    std::size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const std::string_view pair =
            query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            params.emplace(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return params;
}

// Splits a path into segments, rejecting anything containing a traversal
// component. Segment names come from a URL, so they are client-controlled:
// they are only ever used as a hash-map key here (never as a filesystem
// path), but the check is kept as defence in depth.
bool split_path(std::string_view path, std::vector<std::string>& out) {
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const auto slash = path.find('/', pos);
        const std::string_view part =
            path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (!part.empty()) {
            if (part == "." || part == "..") return false;
            if (part.find('\\') != std::string_view::npos) return false;
            out.emplace_back(part);
        }
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return true;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

HttpResponse plain(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.content_type = "text/plain";
    response.body = std::move(body);
    return response;
}

// Parses "bytes=start-end" for a resource of `size` bytes. Returns false for
// a malformed or unsatisfiable range.
bool parse_range(std::string_view header, std::size_t size, std::size_t& start, std::size_t& end) {
    constexpr std::string_view kPrefix = "bytes=";
    if (!header.starts_with(kPrefix)) return false;
    header.remove_prefix(kPrefix.size());
    // Multi-range requests are not supported; a single range covers every
    // real player/CDN use.
    if (header.find(',') != std::string_view::npos) return false;

    const auto dash = header.find('-');
    if (dash == std::string_view::npos) return false;
    const std::string_view first = header.substr(0, dash);
    const std::string_view last = header.substr(dash + 1);

    auto to_number = [](std::string_view text, std::size_t& value) {
        if (text.empty()) return false;
        const auto* begin = text.data();
        const auto* finish = text.data() + text.size();
        const auto result = std::from_chars(begin, finish, value);
        return result.ec == std::errc{} && result.ptr == finish;
    };

    if (first.empty()) {
        // Suffix range: "-N" means the final N bytes.
        std::size_t suffix = 0;
        if (!to_number(last, suffix) || suffix == 0) return false;
        start = suffix >= size ? 0 : size - suffix;
        end = size - 1;
        return true;
    }

    if (!to_number(first, start)) return false;
    if (last.empty()) {
        end = size - 1;
    } else if (!to_number(last, end)) {
        return false;
    }
    if (end >= size) end = size - 1;
    return start <= end && start < size;
}

} // namespace

HlsHttpHandler::HlsHttpHandler(HlsHttpOptions options) : options_(std::move(options)) {}

void HlsHttpHandler::register_stream(const std::string& application, const std::string& stream,
                                     std::shared_ptr<hls::SegmentStore> store) {
    std::lock_guard lock(mutex_);
    streams_[application + "/" + stream].store = std::move(store);
}

void HlsHttpHandler::unregister_stream(const std::string& application, const std::string& stream) {
    std::lock_guard lock(mutex_);
    streams_.erase(application + "/" + stream);
}

std::shared_ptr<hls::SegmentStore> HlsHttpHandler::find_stream(const std::string& application,
                                                               const std::string& stream) const {
    std::lock_guard lock(mutex_);
    const auto it = streams_.find(application + "/" + stream);
    return it == streams_.end() ? nullptr : it->second.store;
}

void HlsHttpHandler::set_renditions(const std::string& application, const std::string& stream,
                                    std::vector<hls::Rendition> renditions) {
    std::lock_guard lock(mutex_);
    streams_[application + "/" + stream].renditions = std::move(renditions);
}

HlsHttpHandler::Stats HlsHttpHandler::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool HlsHttpHandler::authorized(const HttpRequest& request, const std::string& application,
                                const std::string& stream) const {
    if (!options_.require_playback_token) return true;

    const auto params = parse_query(request.query);
    const auto token = params.find("token");
    const auto expires = params.find("expires");
    if (token == params.end() || expires == params.end()) return false;

    std::int64_t expires_at = 0;
    const auto* begin = expires->second.data();
    const auto* finish = begin + expires->second.size();
    if (std::from_chars(begin, finish, expires_at).ec != std::errc{}) return false;

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Same stateless, constant-time verification the RTMP play path uses
    // (management::verify_token) — identical claims, identical secret, so a
    // token works for both protocols or neither.
    return management::verify_token(options_.token_signing_secret, application, stream, token->second,
                                    expires_at, now)
        .ok();
}

void HlsHttpHandler::decorate(HttpResponse& response, const std::string& cache_control) const {
    response.headers["Cache-Control"] = cache_control;
    if (!options_.cors_allow_origin.empty()) {
        response.headers["Access-Control-Allow-Origin"] = options_.cors_allow_origin;
        // Players read these to drive buffering and range logic.
        response.headers["Access-Control-Expose-Headers"] = "Content-Length,Content-Range";
    }
    if (options_.enable_range_requests) response.headers["Accept-Ranges"] = "bytes";
}

HttpResponse HlsHttpHandler::serve_media_playlist(const HttpRequest& request, const StreamEntry& entry,
                                                  const std::string& application,
                                                  const std::string& stream) {
    (void)application;
    (void)stream;
    // Preserve the caller's query string on segment URIs so a token-gated
    // stream stays playable: the player copies the URI verbatim.
    std::string prefix;
    std::string suffix;
    if (!request.query.empty()) suffix = "?" + request.query;

    std::string body = entry.store->playlist(prefix);
    if (!suffix.empty()) {
        // Append the query to each segment line (lines not starting with '#').
        std::string decorated;
        decorated.reserve(body.size() + 64);
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto nl = body.find('\n', pos);
            const std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            decorated += line;
            if (!line.empty() && line.front() != '#') decorated += suffix;
            decorated += "\n";
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        body = std::move(decorated);
    }

    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeM3u8;
    response.body = std::move(body);
    decorate(response, options_.playlist_cache_control);
    return response;
}

HttpResponse HlsHttpHandler::serve_master_playlist(const StreamEntry& entry) {
    if (entry.renditions.empty()) {
        return plain(404, "no renditions declared for this stream");
    }
    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeM3u8;
    response.body = hls::build_master_playlist(entry.renditions);
    decorate(response, options_.master_cache_control);
    return response;
}

HttpResponse HlsHttpHandler::serve_segment(const HttpRequest& request, const StreamEntry& entry,
                                           const std::string& name) {
    auto segment = entry.store->find_segment(name);
    if (!segment) {
        // A segment that has scrolled out of retention. 404 is correct and
        // is what makes bounded cleanup safe for players (they re-fetch the
        // playlist), so it must not be cached.
        auto response = plain(404, "segment not found");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    const auto view = segment->data.view();
    const auto* bytes = reinterpret_cast<const char*>(view.data());
    const std::size_t size = view.size();

    const auto range_header = request.headers.find("range");
    if (options_.enable_range_requests && range_header != request.headers.end()) {
        std::size_t start = 0;
        std::size_t end = 0;
        if (parse_range(range_header->second, size, start, end)) {
            {
                std::lock_guard lock(mutex_);
                stats_.range_requests += 1;
            }
            HttpResponse response;
            response.status = 206;
            response.content_type = kContentTypeMpegTs;
            response.body.assign(bytes + start, end - start + 1);
            decorate(response, options_.segment_cache_control);
            response.headers["Content-Range"] =
                "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
            return response;
        }
        HttpResponse response;
        response.status = 416;
        response.content_type = "text/plain";
        response.body = "requested range not satisfiable";
        response.headers["Content-Range"] = "bytes */" + std::to_string(size);
        return response;
    }

    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeMpegTs;
    // One copy into the response buffer; the stored segment bytes themselves
    // are shared and never duplicated per viewer.
    response.body.assign(bytes, size);
    decorate(response, options_.segment_cache_control);
    return response;
}

HttpResponse HlsHttpHandler::handle(const HttpRequest& request) {
    const std::string& prefix = options_.route_prefix;
    const bool in_prefix = request.path.size() >= prefix.size() &&
                           request.path.compare(0, prefix.size(), prefix) == 0 &&
                           (request.path.size() == prefix.size() || request.path[prefix.size()] == '/');

    if (!in_prefix) {
        // Not ours: hand off to the management API (or 404 if unchained).
        if (next_) return next_(request);
        return plain(404, "not found");
    }

    if (request.method != "GET" && request.method != "HEAD") {
        auto response = plain(405, "method not allowed");
        response.headers["Allow"] = "GET, HEAD";
        return response;
    }

    std::vector<std::string> parts;
    if (!split_path(std::string_view(request.path).substr(prefix.size()), parts) || parts.size() != 3) {
        std::lock_guard lock(mutex_);
        stats_.not_found += 1;
        return plain(404, "not found");
    }

    const std::string& application = parts[0];
    const std::string& stream = parts[1];
    const std::string& resource = parts[2];

    if (!authorized(request, application, stream)) {
        {
            std::lock_guard lock(mutex_);
            stats_.unauthorized += 1;
        }
        auto response = plain(403, "playback token required or invalid");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    StreamEntry entry;
    {
        std::lock_guard lock(mutex_);
        const auto it = streams_.find(application + "/" + stream);
        if (it == streams_.end() || !it->second.store) {
            stats_.not_found += 1;
            auto response = plain(404, "stream not found");
            response.headers["Cache-Control"] = "no-store";
            return response;
        }
        // Copy the entry (a shared_ptr + a small vector) so the rest of the
        // request runs without the registry lock held (3.7).
        entry = it->second;
    }

    HttpResponse response;
    if (resource == "master.m3u8") {
        {
            std::lock_guard lock(mutex_);
            stats_.playlist_requests += 1;
        }
        response = serve_master_playlist(entry);
    } else if (ends_with(resource, ".m3u8")) {
        {
            std::lock_guard lock(mutex_);
            stats_.playlist_requests += 1;
        }
        response = serve_media_playlist(request, entry, application, stream);
    } else if (ends_with(resource, ".ts")) {
        {
            std::lock_guard lock(mutex_);
            stats_.segment_requests += 1;
        }
        response = serve_segment(request, entry, resource);
    } else {
        std::lock_guard lock(mutex_);
        stats_.not_found += 1;
        return plain(404, "not found");
    }

    // HEAD must carry identical headers but no body (RFC 9110). Content-Length
    // is computed from body.size() by the server, so we record it explicitly
    // before clearing.
    if (request.method == "HEAD") {
        response.headers["Content-Length"] = std::to_string(response.body.size());
        response.body.clear();
    }
    return response;
}

} // namespace rtmp_server::control
