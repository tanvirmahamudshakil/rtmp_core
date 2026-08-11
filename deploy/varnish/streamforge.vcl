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

    # This is the public high-scale profile: query strings are neither auth nor
    # viewer identity. Normalizing them prevents cache-buster attacks from
    # creating one object/backend fetch per request.
    if (req.url ~ "\\?") {
        set req.url = regsub(req.url, "\\?.*$", "");
    }
    unset req.http.Cookie;
    unset req.http.Authorization;
    return (hash);
}

sub vcl_backend_fetch {
    # Store one complete immutable segment and satisfy client ranges from it.
    unset bereq.http.Range;
}

sub vcl_backend_response {
    if (bereq.url ~ "\\.ts$") {
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
    } else if (bereq.url ~ "/master\\.m3u8$") {
        if (beresp.status == 200) {
            set beresp.ttl = 30s;
            set beresp.grace = 1m;
            set beresp.keep = 5m;
        } else {
            set beresp.ttl = 0s;
            set beresp.uncacheable = true;
        }
    } else if (bereq.url ~ "\\.m3u8$") {
        if (beresp.status == 200) {
            set beresp.ttl = 1s;
            set beresp.grace = 3s;
            set beresp.keep = 10s;
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
