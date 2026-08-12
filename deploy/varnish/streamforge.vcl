# Managed by StreamForge install-linux.sh
vcl 4.1;

backend default {
    .host = "127.0.0.1";
    .port = "8080";
    .connect_timeout = 1s;
    .first_byte_timeout = 5s;
    .between_bytes_timeout = 5s;
    .max_connections = 1024;
}

sub vcl_recv {
    if (req.method != "GET" && req.method != "HEAD") {
        return (pass);
    }
    if (req.url !~ "^/hls/") {
        return (pass);
    }

    # Playlists carry a per-player session and must reach the origin so each
    # player keeps its own identity. They are tiny; the expensive immutable
    # segment bodies below remain shared by every viewer.
    if (req.url ~ "\.m3u8(?:\?.*)?$") {
        return (pass);
    }
    unset req.http.Cookie;
    unset req.http.Authorization;
    return (hash);
}

sub vcl_hash {
    # Retain the original query in VSL so viewer_estimator can observe the
    # playback session on cache HITs, but deliberately exclude it from the
    # segment cache key. All viewers therefore share one immutable object.
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
    if (bereq.url ~ "\.ts(?:\?.*)?$") {
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
