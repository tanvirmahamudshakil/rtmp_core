#include "rtmp_server/control/hls_http_handler.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <unordered_set>
#include <utility>

#include "rtmp_server/core/random.hpp"
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

constexpr std::string_view kPlaybackSessionParam = "viewer_session";
constexpr std::string_view kPlaybackStreamParam = "viewer_stream";
constexpr std::string_view kSharedCacheParam = "viewer_cache";

bool is_playback_session(std::string_view value) {
    if (value.size() != 32) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Stream/application path components are already split before this is used.
// Percent-encode again when putting one into a query value so '&', '=', '%'
// or whitespace can never manufacture an extra parameter.
std::string percent_encode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

// Remove caller-supplied session metadata before minting a replacement.
// This prevents duplicate parameters from making a player follow redirects
// forever and prevents a forged viewer_stream value from surviving.
std::string query_without_session_params(std::string_view query) {
    std::string out;
    std::size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const auto eq = pair.find('=');
        const auto key = pair.substr(0, eq);
        if (!pair.empty() && key != kPlaybackSessionParam && key != kPlaybackStreamParam &&
            key != kSharedCacheParam) {
            if (!out.empty()) out.push_back('&');
            out.append(pair);
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string playback_session_from_query(std::string_view query) {
    const auto params = parse_query(query);
    const auto it = params.find(std::string(kPlaybackSessionParam));
    return it != params.end() && is_playback_session(it->second) ? it->second : std::string{};
}

// Extracts one named cookie's value from a raw "Cookie" request header, e.g.
// "a=1; rtmp_viewer_id=deadbeef; b=2". No existing cookie parser lives
// anywhere in this codebase (checked src/control, src/protocol), so this is
// a small, deliberately strict one: it only ever runs on the private,
// never-cached redirect path, so a missed/malformed cookie just falls back
// to minting a fresh session — never a security or correctness hazard.
std::string cookie_value(const HttpRequest& request, std::string_view name) {
    const auto it = request.headers.find("cookie");
    if (it == request.headers.end()) return {};
    std::string_view header = it->second;
    std::size_t pos = 0;
    while (pos < header.size()) {
        // Skip leading spaces after ';'.
        while (pos < header.size() && header[pos] == ' ') ++pos;
        const auto semi = header.find(';', pos);
        const std::string_view pair =
            header.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
            return std::string(pair.substr(eq + 1));
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    return {};
}

void append_query_to_playlist_uris(std::string& body, std::string_view query) {
    if (query.empty()) return;
    const std::string suffix = "?" + std::string(query);
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

HlsHttpHandler::HlsHttpHandler(HlsHttpOptions options) : options_(std::move(options)) {
    // A shared playlist object must be identical for every viewer. Enforce the
    // invariant here as well as in production wiring so a future caller cannot
    // accidentally cache one session's query inside another viewer's body.
    if (options_.enable_shared_playlist_cache) {
        options_.propagate_query_to_playlist_uris = false;
    }
}

HlsHttpHandler::StreamEntryPtr HlsHttpHandler::find_entry(const std::string& application,
                                                          const std::string& stream) const {
    std::shared_lock lock(streams_mutex_);
    const auto it = streams_.find(application + "/" + stream);
    return it == streams_.end() ? nullptr : it->second;
}

HlsHttpHandler::StreamEntryPtr HlsHttpHandler::find_or_create_entry(const std::string& application,
                                                                    const std::string& stream) {
    const std::string key = application + "/" + stream;
    {
        std::shared_lock lock(streams_mutex_);
        const auto it = streams_.find(key);
        if (it != streams_.end()) return it->second;
    }
    std::unique_lock lock(streams_mutex_);
    // Re-check: another thread may have created it between the two locks.
    auto& slot = streams_[key];
    if (!slot) slot = std::make_shared<StreamEntry>();
    return slot;
}

void HlsHttpHandler::register_stream(const std::string& application, const std::string& stream,
                                     std::shared_ptr<hls::SegmentStore> store) {
    // Registering a store for a key that already has one (a republish)
    // deliberately keeps the existing entry, so its accumulated byte count
    // and live viewer sessions survive the swap instead of resetting the
    // link's stats to zero mid-audience.
    find_or_create_entry(application, stream)->store.store(std::move(store), std::memory_order_release);
}

void HlsHttpHandler::unregister_stream(const std::string& application, const std::string& stream) {
    std::unique_lock lock(streams_mutex_);
    streams_.erase(application + "/" + stream);
}

std::shared_ptr<hls::SegmentStore> HlsHttpHandler::find_stream(const std::string& application,
                                                               const std::string& stream) const {
    const auto entry = find_entry(application, stream);
    return entry ? entry->store.load(std::memory_order_acquire) : nullptr;
}

void HlsHttpHandler::set_renditions(const std::string& application, const std::string& stream,
                                    std::vector<hls::Rendition> renditions) {
    auto published = std::make_shared<const std::vector<hls::Rendition>>(std::move(renditions));
    find_or_create_entry(application, stream)
        ->renditions.store(std::move(published), std::memory_order_release);
}

HlsHttpHandler::Stats HlsHttpHandler::stats() const {
    // Relaxed throughout: these are independent counters displayed together,
    // never used to order anything, so a snapshot that mixes adjacent
    // increments is both expected and harmless.
    Stats snapshot;
    snapshot.playlist_requests = stats_.playlist_requests.load(std::memory_order_relaxed);
    snapshot.segment_requests = stats_.segment_requests.load(std::memory_order_relaxed);
    snapshot.unauthorized = stats_.unauthorized.load(std::memory_order_relaxed);
    snapshot.not_found = stats_.not_found.load(std::memory_order_relaxed);
    snapshot.range_requests = stats_.range_requests.load(std::memory_order_relaxed);
    return snapshot;
}

void HlsHttpHandler::record_delivery(StreamEntry& entry, std::uint64_t bytes,
                                     std::string_view playback_session) {
    entry.bytes_total.fetch_add(bytes, std::memory_order_relaxed);
    entry.sessions.record(playback_session, std::chrono::steady_clock::now());
}

HlsHttpHandler::LinkStats HlsHttpHandler::link_stats(const std::string& application,
                                                     const std::string& stream) {
    const auto entry = find_entry(application, stream);
    if (!entry) return {};
    LinkStats result;
    result.bytes_total = entry->bytes_total.load(std::memory_order_relaxed);
    result.viewer_count = entry->sessions.active_count(std::chrono::steady_clock::now());
    return result;
}

namespace {
// Reverses the fixed "../" + output_stream + "/index.m3u8" shape every
// renditions-ready callback builds (apps/rtmp_server/main.cpp,
// source_job_manager.cpp) back into the rendition's own stream key. Returns
// empty on anything that doesn't match, which simply drops that rendition
// from the aggregate rather than miscounting it.
std::string rendition_stream_from_uri(const std::string& uri) {
    constexpr std::string_view kPrefix = "../";
    constexpr std::string_view kSuffix = "/index.m3u8";
    if (uri.size() <= kPrefix.size() + kSuffix.size()) return {};
    if (uri.compare(0, kPrefix.size(), kPrefix) != 0) return {};
    if (uri.compare(uri.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return {};
    return uri.substr(kPrefix.size(), uri.size() - kPrefix.size() - kSuffix.size());
}
} // namespace

HlsHttpHandler::LinkStats HlsHttpHandler::aggregate_link_stats(const std::string& application,
                                                               const std::string& master_stream) {
    const auto master = find_entry(application, master_stream);
    if (!master) return {};

    std::vector<std::string> rendition_streams;
    if (const auto renditions = master->renditions.load(std::memory_order_acquire)) {
        for (const auto& rendition : *renditions) {
            auto name = rendition_stream_from_uri(rendition.uri);
            if (!name.empty()) rendition_streams.push_back(std::move(name));
        }
    }
    if (rendition_streams.empty()) return link_stats(application, master_stream);

    // Resolve every rendition's entry first, then read them without holding
    // the registry lock: collect_active() takes each stream's own shard
    // locks, and nesting those under the registry lock would put the
    // (potentially large) union work on the path of every concurrent
    // register/unregister.
    std::vector<StreamEntryPtr> entries;
    entries.reserve(rendition_streams.size());
    {
        std::shared_lock lock(streams_mutex_);
        for (const auto& stream : rendition_streams) {
            const auto it = streams_.find(application + "/" + stream);
            if (it != streams_.end()) entries.push_back(it->second);
        }
    }

    LinkStats aggregate;
    std::unordered_set<std::string> active_sessions;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : entries) {
        aggregate.bytes_total += entry->bytes_total.load(std::memory_order_relaxed);
        // Union rather than sum: one player switching between renditions
        // appears in each variant it touched and must count once.
        entry->sessions.collect_active(active_sessions, now);
    }
    aggregate.viewer_count = active_sessions.size();
    return aggregate;
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

HttpResponse HlsHttpHandler::serve_media_playlist(const HttpRequest& request, hls::SegmentStore& store,
                                                  const std::string& application,
                                                  const std::string& stream) {
    (void)application;
    (void)stream;
    // Preserve the caller's query string on segment URIs so a token-gated
    // stream stays playable: the player copies the URI verbatim.
    std::string body = store.playlist({});
    if (options_.propagate_query_to_playlist_uris) {
        append_query_to_playlist_uris(body, request.query);
    }

    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeM3u8;
    response.body = std::move(body);
    decorate(response, options_.playlist_cache_control);
    return response;
}

HttpResponse HlsHttpHandler::serve_master_playlist(const HttpRequest& request,
                                                   const std::vector<hls::Rendition>& renditions,
                                                   const std::string& application) {
    if (renditions.empty()) {
        return plain(404, "no renditions declared for this stream");
    }
    if (options_.enable_fast_join) {
        // Renditions are always lowest-bandwidth-first (hls/playlist.hpp), so
        // the first entry is exactly the one fast-join wants. Its `uri` is
        // always the fixed "../<output_stream>/index.m3u8" shape every
        // renditions-ready callback builds (main.cpp, source_job_manager.cpp)
        // -- reuse the same reversal the delivery-stats aggregator uses
        // rather than a generic relative-URL resolver, since that's the only
        // shape this ever has to handle.
        const std::string rendition_stream = rendition_stream_from_uri(renditions.front().uri);
        if (!rendition_stream.empty()) {
            HttpResponse redirect;
            redirect.status = 302;
            redirect.content_type = "text/plain";
            std::string location = options_.route_prefix + "/" + application + "/" + rendition_stream + "/index.m3u8";
            if (!request.query.empty()) location += "?" + request.query;
            redirect.headers["Location"] = location;
            // Same reasoning as the playback-session redirect: this response
            // is per-request routing, not shared content, so it must never
            // be cached (and in shared-cache mode, master.m3u8 itself is a
            // cached object -- this redirect only ever fires on the fresh,
            // un-cached bootstrap request that reaches serve_master_playlist,
            // never on a cache hit).
            redirect.headers["Cache-Control"] = "private, no-store";
            if (!options_.cors_allow_origin.empty()) {
                redirect.headers["Access-Control-Allow-Origin"] = options_.cors_allow_origin;
            }
            return redirect;
        }
        // Fall through to the normal master playlist if the URI shape ever
        // doesn't match (defensive -- shouldn't happen given how renditions
        // are always registered).
    }
    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeM3u8;
    response.body = hls::build_master_playlist(renditions);
    if (options_.propagate_query_to_playlist_uris) {
        append_query_to_playlist_uris(response.body, request.query);
    }
    decorate(response,
             options_.enable_playback_sessions && !options_.enable_shared_playlist_cache
                 ? "private, no-store"
                 : options_.master_cache_control);
    return response;
}

HttpResponse HlsHttpHandler::serve_segment(const HttpRequest& request, hls::SegmentStore& store,
                                           const std::string& name) {
    auto segment = store.find_segment(name);
    if (!segment) {
        // A segment that has scrolled out of retention. 404 is correct and
        // is what makes bounded cleanup safe for players (they re-fetch the
        // playlist), so it must not be cached.
        auto response = plain(404, "segment not found");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    const auto view = segment->data.view();
    const std::size_t size = view.size();

    const auto range_header = request.headers.find("range");
    if (options_.enable_range_requests && range_header != request.headers.end()) {
        std::size_t start = 0;
        std::size_t end = 0;
        if (parse_range(range_header->second, size, start, end)) {
            stats_.range_requests.fetch_add(1, std::memory_order_relaxed);
            HttpResponse response;
            response.status = 206;
            response.content_type = kContentTypeMpegTs;
            response.shared_body = segment->data;
            response.shared_body_offset = start;
            response.shared_body_length = end - start + 1;
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
    response.shared_body = segment->data;
    response.shared_body_length = size;
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
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    const std::string& application = parts[0];
    const std::string& stream = parts[1];
    const std::string& resource = parts[2];

    // A disabled stream serves no playlists or segments: its .m3u8 links stop
    // working the moment it is disabled, in lockstep with the RTMP gate.
    if (enabled_checker_ && !enabled_checker_(application, stream)) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    if (!authorized(request, application, stream)) {
        stats_.unauthorized.fetch_add(1, std::memory_order_relaxed);
        auto response = plain(403, "playback token required or invalid");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    // One shared-lock registry lookup, then a reference count: everything
    // below runs with no registry lock held, so concurrent requests never
    // serialise on the shape of the stream table (3.7).
    const StreamEntryPtr entry = find_entry(application, stream);
    // A master playlist name (e.g. a source-transcode job's output base
    // name) only ever gets a `renditions` entry -- its segments live under
    // each rendition's own stream key, so it never has a `store`. Only
    // media-playlist/segment requests need one.
    const bool needs_store = resource != "master.m3u8";
    // Read the store once and reuse that value for the whole request: a
    // republish could swap it underneath us, and serving half a request
    // from each store would be worse than serving all of it from the one
    // that was live when the request arrived.
    const auto store = entry ? entry->store.load(std::memory_order_acquire) : nullptr;
    if (!entry || (needs_store && !store)) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        auto response = plain(404, "stream not found");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    // A public, stable HLS URL starts a fresh playback session on each open.
    // Normal/private mode propagates that query through child URIs. Shared
    // mode adds viewer_cache=1 to the redirect but keeps playlist bodies
    // query-free; Varnish logs the session-bearing playlist URL while safely
    // serving the same body object to every viewer.
    if (options_.enable_playback_sessions && ends_with(resource, ".m3u8")) {
        const auto params = parse_query(request.query);
        const auto session_it = params.find(std::string(kPlaybackSessionParam));
        const auto stream_it = params.find(std::string(kPlaybackStreamParam));
        const auto shared_cache_it = params.find(std::string(kSharedCacheParam));
        const bool valid_session = session_it != params.end() && is_playback_session(session_it->second);
        const bool valid_stream = stream_it != params.end() && !stream_it->second.empty();
        const bool valid_cache_route = !options_.enable_shared_playlist_cache ||
                                       (shared_cache_it != params.end() && shared_cache_it->second == "1");
        if (!valid_session || !valid_stream || !valid_cache_route) {
            // Recognise a returning viewer by its persistent cookie before
            // minting a brand-new random session. This request arrives here
            // precisely because its URL carries no viewer_session — in
            // shared-cache mode that's true of every hop that follows an
            // undecorated rendition link out of the cached master body, so
            // without this check the same returning viewer would mint a
            // fresh session (and look like a brand-new viewer) on every such
            // hop. The VCL only strips Cookie on requests it hashes into the
            // shared cache; this bare-query request takes the pass branch
            // and reaches here with its Cookie header intact.
            const std::string existing_cookie = cookie_value(request, options_.viewer_cookie_name);
            const bool reuse_cookie = is_playback_session(existing_cookie);
            const std::string session = reuse_cookie ? existing_cookie
                                       : options_.playback_session_id_factory
                                             ? options_.playback_session_id_factory()
                                             : core::generate_secure_token(16);
            if (!is_playback_session(session)) {
                auto response = plain(500, "playback session generator failed");
                response.headers["Cache-Control"] = "no-store";
                return response;
            }
            std::string query = query_without_session_params(request.query);
            if (!query.empty()) query.push_back('&');
            query += std::string(kPlaybackSessionParam) + "=" + session;
            query += "&" + std::string(kPlaybackStreamParam) + "=" + percent_encode(stream);
            if (options_.enable_shared_playlist_cache) {
                query += "&" + std::string(kSharedCacheParam) + "=1";
            }
            HttpResponse redirect;
            redirect.status = 302;
            redirect.content_type = "text/plain";
            redirect.headers["Location"] = request.path + "?" + query;
            // Per-viewer state (a Set-Cookie, and the Location itself embeds
            // this viewer's session) must never end up in the shared cache.
            // This response is only ever produced on the un-hashed VCL pass
            // path (see options_.viewer_cookie_name doc comment), so it is
            // safe here, but keep the header explicit and defensive anyway.
            redirect.headers["Cache-Control"] = "private, no-store";
            if (!options_.cors_allow_origin.empty()) {
                redirect.headers["Access-Control-Allow-Origin"] = options_.cors_allow_origin;
            }
            if (!reuse_cookie) {
                // Only set the cookie when minting a new session: refreshing
                // it on every reuse would just be extra bytes for no benefit,
                // since the value never changes until it expires.
                // cors_allow_origin being set means cross-origin players
                // (hls.js embedded on another site) are an expected caller,
                // and a cross-site fetch only carries a cookie tagged
                // SameSite=None. None is only legal (browsers accept it) when
                // paired with Secure, so fall back to Lax whenever the
                // deployment can't mark the cookie Secure — same-origin
                // callers still get it, cross-origin ones simply re-mint a
                // session each time, same as before this fix.
                const bool cross_origin_capable =
                    !options_.cors_allow_origin.empty() && options_.viewer_cookie_secure;
                std::string cookie = options_.viewer_cookie_name + "=" + session +
                                     "; Path=" + options_.route_prefix + "; HttpOnly; SameSite=" +
                                     (cross_origin_capable ? "None" : "Lax") +
                                     "; Max-Age=" + std::to_string(options_.viewer_cookie_max_age.count());
                if (options_.viewer_cookie_secure) cookie += "; Secure";
                redirect.headers["Set-Cookie"] = std::move(cookie);
            }
            return redirect;
        }
    }

    HttpResponse response;
    bool countable = false;
    if (resource == "master.m3u8") {
        stats_.playlist_requests.fetch_add(1, std::memory_order_relaxed);
        const auto renditions = entry->renditions.load(std::memory_order_acquire);
        static const std::vector<hls::Rendition> kNoRenditions;
        response = serve_master_playlist(request, renditions ? *renditions : kNoRenditions, application);
    } else if (ends_with(resource, ".m3u8")) {
        stats_.playlist_requests.fetch_add(1, std::memory_order_relaxed);
        response = serve_media_playlist(request, *store, application, stream);
        countable = true;
    } else if (ends_with(resource, ".ts")) {
        stats_.segment_requests.fetch_add(1, std::memory_order_relaxed);
        response = serve_segment(request, *store, resource);
        countable = true;
    } else {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    // Media-playlist and segment requests are the recurring "still watching"
    // signal (a player refetches one or the other every segment duration);
    // master.m3u8 is fetched once at session start and would undercount a
    // long-running viewer, so it deliberately isn't counted here.
    if (options_.track_delivery_stats && countable &&
        (response.status == 200 || response.status == 206)) {
        record_delivery(*entry, response.payload_size(), playback_session_from_query(request.query));
    }

    // HEAD must carry identical headers but no body (RFC 9110). Content-Length
    // is computed from body.size() by the server, so we record it explicitly
    // before clearing.
    if (request.method == "HEAD") {
        response.headers["Content-Length"] = std::to_string(response.payload_size());
        response.body.clear();
        response.shared_body = {};
        response.shared_body_offset = 0;
        response.shared_body_length = 0;
    }
    return response;
}

} // namespace rtmp_server::control
