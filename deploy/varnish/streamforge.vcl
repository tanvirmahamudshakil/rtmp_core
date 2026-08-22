# Managed by StreamForge install-linux.sh
vcl 4.1;

backend default {
    .host = "127.0.0.1";
    .port = "8080";
    .connect_timeout = 1s;
    .first_byte_timeout = 5s;
    .between_bytes_timeout = 5s;
    # A bounded burst ceiling; steady-state playlist traffic is collapsed by
    # the micro-cache below instead of consuming one connection per viewer.
    .max_connections = 4096;
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

    if (bereq.url ~ "/master\.m3u8(?:\?.*)?$") {
        if (beresp.status == 200 && beresp.http.Cache-Control ~ "(?i)public") {
            set beresp.ttl = 30s;
            set beresp.grace = 1m;
            set beresp.keep = 5m;
            set beresp.do_stream = false;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } elseif (bereq.url ~ "\.m3u8(?:\?.*)?$") {
        if (beresp.status == 200 && beresp.http.Cache-Control ~ "(?i)public") {
            set beresp.ttl = 1s;
            set beresp.grace = 5s;
            set beresp.keep = 30s;
            # Collapse synchronized player polls into one complete response.
            set beresp.do_stream = false;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } elseif (bereq.url ~ "\.ts(?:\?.*)?$") {
        if (beresp.status == 200) {
            set beresp.ttl = 1h;
            set beresp.grace = 5m;
            set beresp.keep = 1h;
            # Coalesce a hot segment's initial burst into one origin fetch.
            set beresp.do_stream = false;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
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
