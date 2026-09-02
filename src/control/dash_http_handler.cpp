#include "rtmp_server/control/dash_http_handler.hpp"

#include <algorithm>
#include <charconv>

#include "rtmp_server/core/hmac.hpp"
#include "rtmp_server/core/random.hpp"

namespace rtmp_server::control {

namespace {

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

// Same traversal-safe splitter as HlsHttpHandler's (kept as a private copy
// rather than a shared header: both are ~15 lines and neither delivery
// surface should have to take a dependency on the other to stay correct).
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

bool parse_range(std::string_view header, std::size_t size, std::size_t& start, std::size_t& end) {
    constexpr std::string_view kPrefix = "bytes=";
    if (!header.starts_with(kPrefix)) return false;
    header.remove_prefix(kPrefix.size());
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

DashHttpHandler::StreamEntryPtr DashHttpHandler::find_entry(const std::string& application,
                                                             const std::string& stream) const {
    std::shared_lock lock(streams_mutex_);
    const auto it = streams_.find(application + "/" + stream);
    return it == streams_.end() ? nullptr : it->second;
}

DashHttpHandler::StreamEntryPtr DashHttpHandler::find_or_create_entry(const std::string& application,
                                                                       const std::string& stream) {
    const std::string key = application + "/" + stream;
    {
        std::shared_lock lock(streams_mutex_);
        const auto it = streams_.find(key);
        if (it != streams_.end()) return it->second;
    }
    std::unique_lock lock(streams_mutex_);
    auto& slot = streams_[key];
    if (!slot) slot = std::make_shared<StreamEntry>();
    return slot;
}

void DashHttpHandler::register_representation(const std::string& application, const std::string& stream,
                                               const std::string& representation_id,
                                               std::shared_ptr<dash::SegmentStore> store) {
    const auto entry = find_or_create_entry(application, stream);
    std::unique_lock lock(entry->mutex);
    auto& slot = entry->representations[representation_id];
    slot.store = std::move(store);
}

void DashHttpHandler::unregister_stream(const std::string& application, const std::string& stream) {
    std::unique_lock lock(streams_mutex_);
    streams_.erase(application + "/" + stream);
}

void DashHttpHandler::set_representations(const std::string& application, const std::string& stream,
                                          std::vector<dash::Representation> representations,
                                          std::uint32_t timescale, std::uint64_t segment_duration) {
    const auto entry = find_or_create_entry(application, stream);
    entry->timescale.store(timescale, std::memory_order_relaxed);
    entry->segment_duration.store(segment_duration, std::memory_order_relaxed);
    std::unique_lock lock(entry->mutex);
    for (auto& rep : representations) {
        auto& slot = entry->representations[rep.id];
        slot.descriptor = std::move(rep);
    }
}

void DashHttpHandler::decorate(HttpResponse& response, const std::string& cache_control) const {
    response.headers["Cache-Control"] = cache_control;
    if (!options_.cors_allow_origin.empty()) {
        response.headers["Access-Control-Allow-Origin"] = options_.cors_allow_origin;
        response.headers["Access-Control-Expose-Headers"] = "Content-Length,Content-Range";
    }
    if (options_.enable_range_requests) response.headers["Accept-Ranges"] = "bytes";
}

HttpResponse DashHttpHandler::serve_manifest(const StreamEntryPtr& entry) {
    std::vector<dash::Representation> reps;
    std::uint64_t start_number = 0;
    bool ended = false;
    bool any_store = false;
    {
        std::shared_lock lock(entry->mutex);
        reps.reserve(entry->representations.size());
        for (const auto& [id, rep] : entry->representations) {
            reps.push_back(rep.descriptor);
            if (rep.store) {
                any_store = true;
                // Every representation is cut on the same wall-clock cadence
                // (docs/dash.md), so any one store's window bounds are
                // representative of the whole manifest's addressable range.
                start_number = std::max(start_number, rep.store->start_number());
                ended = ended || rep.store->ended();
            }
        }
    }
    if (!any_store) return plain(404, "no representations declared for this stream");

    dash::MpdOptions options;
    options.timescale = entry->timescale.load(std::memory_order_relaxed);
    options.segment_duration = entry->segment_duration.load(std::memory_order_relaxed);
    options.start_number = start_number;
    options.is_static = ended;
    options.suggested_presentation_delay_seconds = options_.suggested_presentation_delay_seconds;
    options.minimum_update_period_seconds = options_.minimum_update_period_seconds;
    if (options.timescale > 0 && options.segment_duration > 0) {
        options.time_shift_buffer_depth_seconds =
            static_cast<double>(options.segment_duration) / static_cast<double>(options.timescale) * 6.0;
    }

    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeMpd;
    response.body = dash::build_mpd(reps, options);
    decorate(response, options_.playlist_cache_control);
    return response;
}

HttpResponse DashHttpHandler::serve_init(dash::SegmentStore& store) {
    const auto init = store.current_init();
    if (!init) {
        auto response = plain(404, "init segment not yet available");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }
    HttpResponse response;
    response.status = 200;
    response.content_type = kContentTypeMp4;
    response.shared_body = init->data;
    response.shared_body_length = init->data.size();
    // Immutable while this epoch is current, but a codec-parameter change
    // republishes a new body at this SAME url -- unlike a media segment's
    // name, the init URL has no epoch in it. Serve it as revalidatable
    // rather than permanently immutable so a rebuild is actually observed.
    decorate(response, "public, max-age=3600, must-revalidate");
    return response;
}

HttpResponse DashHttpHandler::serve_segment(const HttpRequest& request, dash::SegmentStore& store,
                                            const std::string& name) {
    auto segment = store.find_segment(name);
    if (!segment) {
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
            HttpResponse response;
            response.status = 206;
            response.content_type = kContentTypeMp4;
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
    response.content_type = kContentTypeMp4;
    response.shared_body = segment->data;
    response.shared_body_length = size;
    decorate(response, options_.segment_cache_control);
    return response;
}

HttpResponse DashHttpHandler::handle(const HttpRequest& request) {
    const std::string& prefix = options_.route_prefix;
    const bool in_prefix = request.path.size() >= prefix.size() &&
                           request.path.compare(0, prefix.size(), prefix) == 0 &&
                           (request.path.size() == prefix.size() || request.path[prefix.size()] == '/');
    if (!in_prefix) {
        if (next_) return next_(request);
        return plain(404, "not found");
    }

    if (request.method != "GET" && request.method != "HEAD") {
        auto response = plain(405, "method not allowed");
        response.headers["Allow"] = "GET, HEAD";
        return response;
    }

    if (!options_.edge_fetch_secret.empty()) {
        const auto it = request.headers.find(options_.edge_fetch_header);
        const bool ok = it != request.headers.end() &&
                        core::constant_time_equals(it->second, options_.edge_fetch_secret);
        if (!ok) {
            stats_.edge_unauthorized.fetch_add(1, std::memory_order_relaxed);
            auto response = plain(403, "edge token required");
            response.headers["Cache-Control"] = "no-store";
            return response;
        }
    }

    std::vector<std::string> parts;
    if (!split_path(std::string_view(request.path).substr(prefix.size()), parts) ||
        (parts.size() != 3 && parts.size() != 4)) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    const std::string& application = parts[0];
    const std::string& stream = parts[1];

    if (enabled_checker_ && !enabled_checker_(application, stream)) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    const auto entry = find_entry(application, stream);
    if (!entry) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        auto response = plain(404, "stream not found");
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    HttpResponse response;
    if (parts.size() == 3 && parts[2] == "manifest.mpd") {
        stats_.manifest_requests.fetch_add(1, std::memory_order_relaxed);
        response = serve_manifest(entry);
    } else if (parts.size() == 4) {
        const std::string& representation_id = parts[2];
        const std::string& resource = parts[3];

        std::shared_ptr<dash::SegmentStore> store;
        {
            std::shared_lock lock(entry->mutex);
            const auto it = entry->representations.find(representation_id);
            if (it != entry->representations.end()) store = it->second.store;
        }
        if (!store) {
            stats_.not_found.fetch_add(1, std::memory_order_relaxed);
            auto not_found = plain(404, "representation not found");
            not_found.headers["Cache-Control"] = "no-store";
            return not_found;
        }

        if (resource == "init.mp4") {
            response = serve_init(*store);
        } else if (ends_with(resource, ".m4s")) {
            stats_.segment_requests.fetch_add(1, std::memory_order_relaxed);
            response = serve_segment(request, *store, resource);
        } else {
            stats_.not_found.fetch_add(1, std::memory_order_relaxed);
            return plain(404, "not found");
        }
    } else {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return plain(404, "not found");
    }

    if (request.method == "HEAD") {
        response.headers["Content-Length"] = std::to_string(response.payload_size());
        response.body.clear();
        response.shared_body = {};
        response.shared_body_offset = 0;
        response.shared_body_length = 0;
    }
    return response;
}

DashHttpHandler::Stats DashHttpHandler::stats() const {
    Stats snapshot;
    snapshot.manifest_requests = stats_.manifest_requests.load(std::memory_order_relaxed);
    snapshot.segment_requests = stats_.segment_requests.load(std::memory_order_relaxed);
    snapshot.not_found = stats_.not_found.load(std::memory_order_relaxed);
    snapshot.edge_unauthorized = stats_.edge_unauthorized.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace rtmp_server::control
