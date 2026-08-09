# Managed by StreamForge install-linux.sh
vcl 4.1;

backend default {
    .host = "127.0.0.1";
    .port = "8080";
}

sub vcl_recv {
    if (req.method != "GET" && req.method != "HEAD") {
        return (pass);
    }
    if (req.url ~ "\.m3u8(\?.*)?$") {
        # Session/token-bearing playlists are private and their bodies contain
        # per-player query values, so they must never be shared.
        if (req.url ~ "\?") {
            return (pass);
        }
        # A public playlist falls through to the 1s micro-cache. A fresh
        # master request now returns an uncacheable session redirect.
    }
    return (hash);
}

sub vcl_hash {
    if (req.url ~ "\.ts\?" &&
        req.url ~ "[?&]viewer_session=[0-9a-f]{32}&viewer_stream=[^&]*$") {
        # Session identity stays in the client access log but not the object
        # key. Earlier query fields such as token/expires remain hashed.
        hash_data(regsub(req.url,
            "[?&]viewer_session=[0-9a-f]{32}&viewer_stream=[^&]*$", ""));
    } else {
        hash_data(req.url);
    }
    if (req.http.host) {
        hash_data(req.http.host);
    } else {
        hash_data(server.ip);
    }
    return (lookup);
}

sub vcl_backend_fetch {
    # Cache the complete immutable segment, never a viewer's partial range.
    unset bereq.http.Range;
}

sub vcl_backend_response {
    if (bereq.url ~ "\.ts(\?.*)?$") {
        if (beresp.status == 200) {
            set beresp.ttl = 2m;
            set beresp.grace = 10s;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } else if (bereq.url ~ "\.m3u8(\?.*)?$") {
        if (beresp.status == 200) {
            set beresp.ttl = 1s;
            set beresp.grace = 2s;
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
