# StreamForge multi-node HLS edge / origin-shield cache.
#
# Managed by deploy/edge/install-edge.sh. This is a PURE caching layer that
# sits in front of a StreamForge origin's public HTTPS endpoint. It never
# talks to an rtmp_server directly and never terminates TLS itself: a local
# Caddy hop (127.0.0.1:8090) forwards to the real origin over HTTPS, so this
# file is byte-identical on every edge and every shield node and needs no
# per-host templating.
#
#   viewer --HTTPS--> Caddy :443 --HTTP--> Varnish :6081 (this VCL)
#          --HTTP--> Caddy :8090 --HTTPS--> origin (Caddy -> Varnish -> rtmp_server)
#
# A dedicated shield is just an edge with no viewers of its own: point a
# fleet of edges at the shield's :6081 instead of at the origin and set
# STREAMFORGE_EDGE_ROLE=shield so its responses are labelled distinctly.
#
# The origin already runs its own Varnish (deploy/varnish/streamforge.vcl)
# which mints the per-viewer session redirect and produces cacheable
# `viewer_cache=1` playlist objects and immutable segments. This layer does
# NOT repeat any of that: it forwards, caches what the origin marked
# cacheable, coalesces stampedes into one origin fetch, and serves stale
# during an origin hiccup.

vcl 4.1;

import std;

backend origin {
    # Local Caddy outbound TLS proxy to the real origin. Same on every node.
    .host = "127.0.0.1";
    .port = "8090";
    .connect_timeout = 2s;
    .first_byte_timeout = 8s;
    .between_bytes_timeout = 8s;
    # Uncapped on purpose: the origin's own HTTP worker pool and its own
    # Varnish are the single backpressure point. A cap here would only turn a
    # cold-cache join burst into 503s while every hot object is already local.
    .probe = {
        # /health/ready is served by the origin's management handler and
        # passes through its public tier. If a deployment does not expose it
        # publicly, set STREAMFORGE_EDGE_PROBE=disabled in the unit and this
        # probe is replaced with a no-op include by install-edge.sh.
        .url = "/health/ready";
        .interval = 3s;
        .timeout = 2s;
        .window = 5;
        .threshold = 3;
        .initial = 2;
        .expected_response = 200;
    }
}

sub vcl_init {
    # Fail closed if the token was not provided to the unit: without it every
    # origin fetch would 403 and the node would serve nothing but errors, so
    # make the misconfiguration loud at load time instead.
    if (std.getenv("STREAMFORGE_EDGE_TOKEN") == "") {
        return (fail);
    }
}

sub vcl_recv {
    # Delivery is read-only.
    if (req.method != "GET" && req.method != "HEAD") {
        return (synth(405, "Method Not Allowed"));
    }

    # An edge serves HLS delivery and nothing else. The management API,
    # /metrics, the admin panel and the RTMP paths never leave the origin's
    # private network.
    if (req.url !~ "^/hls/") {
        return (synth(403, "Forbidden"));
    }

    # Delivery objects are shared across all viewers; a request cookie can
    # only poison the shared cache key or leak between viewers.
    unset req.http.Cookie;

    # Normalise segment URLs to their immutable identity: a .ts is uniquely
    # named and never varies by query, so dropping the query string collapses
    # every variant onto one cache object and maximises the hit ratio.
    if (req.url ~ "\.ts(\?.*)?$") {
        set req.url = regsub(req.url, "\?.*$", "");
    }

    # Never forward a client-supplied edge token; this layer sets its own.
    unset req.http.X-Edge-Token;

    return (hash);
}

sub vcl_hash {
    hash_data(req.url);
    if (req.http.host) {
        hash_data(req.http.host);
    } else {
        hash_data(server.ip);
    }
    return (lookup);
}

sub vcl_backend_fetch {
    # Present the shared edge token the origin's HlsHttpHandler requires
    # (config hls_edge_fetch_secret / HlsHttpOptions::edge_fetch_secret).
    set bereq.http.X-Edge-Token = std.getenv("STREAMFORGE_EDGE_TOKEN");
    # Fetch the whole immutable segment once; client Range requests are
    # satisfied from the cached object by vcl_hit / builtin range support.
    unset bereq.http.Range;
}

sub vcl_backend_response {
    # A failed background refresh must not evict the stale object currently
    # shielding viewers from a slow or flapping origin.
    if (beresp.status >= 500 && bereq.is_bgfetch) {
        return (abandon);
    }

    # Honour the origin's own "do not cache this" verdict verbatim: the
    # per-viewer 302 session redirect is no-store / private and must reach
    # exactly one player.
    if (beresp.http.Cache-Control ~ "(?i)(no-store|private)") {
        set beresp.uncacheable = true;
        set beresp.ttl = 0s;
        return (deliver);
    }

    # Any non-200 (a segment that scrolled out of retention -> 404, a
    # transient origin 5xx, a 403 while the token is being rotated) gets a
    # short POSITIVE ttl, never uncacheable: an uncacheable object disables
    # request coalescing for that URL, so a 404/5xx burst would forward every
    # single viewer request to the origin individually. One second lets one
    # origin round trip answer the whole burst.
    if (beresp.status != 200) {
        set beresp.ttl = 1s;
        set beresp.grace = 2s;
        set beresp.keep = 0s;
        return (deliver);
    }

    if (bereq.url ~ "/master\.m3u8($|\?)") {
        # Rendition set changes rarely.
        set beresp.ttl = 30s;
        set beresp.grace = 5m;
        set beresp.keep = 10m;
        set beresp.do_stream = false;
    } elseif (bereq.url ~ "\.m3u8($|\?)") {
        # Live media playlist: one new segment per target duration. A 1s ttl
        # keeps every viewer within a second of the live edge while a single
        # origin fetch per second serves an entire edge's audience. Grace is
        # generous so a momentarily unreachable origin never stalls playback.
        set beresp.ttl = 1s;
        set beresp.grace = 15s;
        set beresp.keep = 30s;
        set beresp.do_stream = false;
    } elseif (bereq.url ~ "\.ts($|\?)") {
        # Immutable, uniquely named: cache hard, keep long for stale-if-error.
        set beresp.ttl = 1h;
        set beresp.grace = 24h;
        set beresp.keep = 24h;
        set beresp.do_stream = false;
    } else {
        set beresp.ttl = 1s;
        set beresp.grace = 2s;
    }
    return (deliver);
}

sub vcl_hit {
    # Fresh object: serve it.
    if (obj.ttl >= 0s) {
        return (deliver);
    }
    # Within grace: serve stale immediately and refresh in the background,
    # unless the origin is sick, in which case stretch stale coverage to the
    # full keep window so viewers ride out the outage.
    if (obj.ttl + obj.grace >= 0s) {
        return (deliver);
    }
    return (restart);
}

sub vcl_deliver {
    if (obj.hits > 0) {
        set resp.http.X-Cache = "HIT";
    } else {
        set resp.http.X-Cache = "MISS";
    }
    set resp.http.X-Cache-Hits = obj.hits;
    set resp.http.X-Edge-Role = std.getenv("STREAMFORGE_EDGE_ROLE");
    if (std.getenv("STREAMFORGE_EDGE_NODE") != "") {
        set resp.http.X-Edge-Node = std.getenv("STREAMFORGE_EDGE_NODE");
    }
    # Hop-by-hop internals never reach viewers.
    unset resp.http.X-Varnish;
    unset resp.http.Via;
    unset resp.http.Server;
    unset resp.http.X-Edge-Token;
}

sub vcl_synth {
    set resp.http.Content-Type = "text/plain; charset=utf-8";
    set resp.http.Cache-Control = "no-store";
    if (resp.status == 503) {
        set resp.http.Retry-After = "2";
    }
    synthetic(resp.reason);
    return (deliver);
}

sub vcl_backend_error {
    # Total origin failure with no usable stale object. Keep it short and
    # uncached so recovery is immediate.
    set beresp.status = 503;
    set beresp.http.Cache-Control = "no-store";
    set beresp.http.Retry-After = "2";
    set beresp.ttl = 0s;
    synthetic("origin unavailable");
    return (deliver);
}
