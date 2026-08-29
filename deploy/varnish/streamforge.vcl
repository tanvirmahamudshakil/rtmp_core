# Managed by StreamForge install-linux.sh
vcl 4.1;

backend default {
    .host = "127.0.0.1";
    .port = "8080";
    .connect_timeout = 1s;
    .first_byte_timeout = 5s;
    .between_bytes_timeout = 5s;
    # No .max_connections: the Varnish->origin fan-in is deliberately
    # uncapped. The origin's own HTTP worker pool (apps/rtmp_server/main.cpp,
    # sized from the core count with an unbounded accept queue) is the single
    # backpressure point; a cap here would only convert a join stampede into
    # 503s while every segment/playlist body is already cache-hot.
}

sub vcl_recv {
    if (req.method != "GET" && req.method != "HEAD") {
        return (pass);
    }
    if (req.url !~ "^/hls/") {
        return (pass);
    }

    # A fresh playlist URL has no viewer_cache marker and reaches the origin
    # once to receive its private per-player session redirect. The redirected
    # URL is safe to micro-cache: its response body contains no session/query
    # data, while the original URL remains visible to VSL edge accounting.
    if (req.url ~ "\.m3u8(?:\?.*)?$") {
        if (req.url !~ "[?&]viewer_cache=1(?:&|$)") {
            return (pass);
        }
        unset req.http.Cookie;
        unset req.http.Authorization;
        return (hash);
    }
    unset req.http.Cookie;
    unset req.http.Authorization;
    return (hash);
}

sub vcl_hash {
    # Retain the original query in VSL so viewer_estimator can observe the
    # playback session on playlist HITs, but deliberately exclude it from the
    # shared playlist/segment cache key.
    hash_data(regsub(req.url, "\?.*$", ""));
    if (req.http.host) {
        hash_data(req.http.host);
    } else {
        hash_data(server.ip);
    }
    return (lookup);
}

sub vcl_backend_fetch {
    # Store one complete immutable segment and satisfy client ranges from it.
    unset bereq.http.Range;
}

sub vcl_backend_response {
    # A failed background refresh must not evict the short stale playlist that
    # is currently shielding players from a transient origin overload.
    if (beresp.status >= 500 && bereq.is_bgfetch) {
        return (abandon);
    }

    # Per-viewer responses stay uncacheable no matter their status: the
    # session-assigning 302 redirect carries a Location/Set-Cookie unique to
    # one player and must never be served to another.
    if (beresp.http.Cache-Control ~ "(?i)(no-store|private)") {
        set beresp.uncacheable = true;
        set beresp.ttl = 0s;
        return (deliver);
    }

    # Everything else that is NOT a 200 (404 for a segment that scrolled out
    # of retention, 5xx during an origin hiccup) MUST get a short positive
    # TTL, not `uncacheable = true`. An uncacheable object disables Varnish
    # request coalescing for that URL, so during a 404/5xx burst every single
    # player request is forwarded to the origin individually -- the exact
    # thundering herd the cache exists to prevent. A 1s cache lets one origin
    # request answer the whole burst.
    if (beresp.status != 200) {
        set beresp.ttl = 1s;
        set beresp.grace = 1s;
        set beresp.keep = 0s;
        return (deliver);
    }

    if (bereq.url ~ "/master\.m3u8(?:\?.*)?$") {
        if (beresp.http.Cache-Control ~ "(?i)public") {
            set beresp.ttl = 30s;
            set beresp.grace = 1m;
            set beresp.keep = 5m;
            set beresp.do_stream = false;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } elseif (bereq.url ~ "\.m3u8(?:\?.*)?$") {
        if (beresp.http.Cache-Control ~ "(?i)public") {
            # Media playlist changes once per segment (6 s). A 3 s TTL is
            # always within one segment of fresh yet halves origin playlist
            # fetches versus a 1 s TTL; grace covers a slow origin refresh.
            set beresp.ttl = 3s;
            set beresp.grace = 9s;
            set beresp.keep = 30s;
            # Collapse synchronized player polls into one complete response.
            set beresp.do_stream = false;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } elseif (bereq.url ~ "\.ts(?:\?.*)?$") {
        set beresp.ttl = 1h;
        set beresp.grace = 5m;
        set beresp.keep = 1h;
        # Coalesce a hot segment's initial burst into one origin fetch.
        set beresp.do_stream = false;
    } else {
        set beresp.ttl = 0s;
        set beresp.uncacheable = true;
    }
    return (deliver);
}

sub vcl_deliver {
    unset resp.http.X-Varnish;
    unset resp.http.Via;
    unset resp.http.Server;
}
