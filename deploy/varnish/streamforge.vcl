# Managed by StreamForge install-linux.sh
vcl 4.1;

import digest;

# Cookie/query-param names and the session-id shape must stay in lock-step
# with HlsHttpHandler (src/control/hls_http_handler.cpp,
# include/rtmp_server/control/hls_http_handler.hpp): kPlaybackSessionParam,
# kPlaybackStreamParam, kSharedCacheParam, viewer_cookie_name, and
# is_playback_session()'s 32-lowercase-hex-char format. Update both sides
# together if any of those ever change.

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

    # A fresh playlist URL has no viewer_cache marker. This used to `pass` to
    # the origin so HlsHttpHandler could mint the private per-player session
    # redirect (see hls_http_handler.cpp's enable_playback_sessions block);
    # every simultaneous join at once meant every one of those redirects hit
    # the origin's HTTP path at once, producing no cacheable content and
    # existing purely to hand back a Location header. Minting it here removes
    # the origin from a viewer's join path entirely -- see vcl_synth below,
    # which builds byte-for-byte the same redirect shape HlsHttpHandler does.
    # The redirected URL (carrying viewer_cache=1) is what actually gets
    # cached below.
    if (req.url ~ "\.m3u8(?:\?.*)?$") {
        if (req.url !~ "[?&]viewer_cache=1(?:&|$)") {
            # Only a well-formed "/hls/<app>/<stream>/<resource>" URL can be
            # turned into a valid redirect here; anything else (an
            # already-malformed request, or a future URL shape this VCL
            # doesn't know about) falls through to the origin, which owns the
            # real 404/validation logic. Guessing wrong here would mint a
            # redirect to a broken URL instead of the origin's honest error.
            if (req.url !~ "^/hls/[^/]+/[^/]+/[^/?]+(?:\?.*)?$") {
                return (pass);
            }

            # This id is NOT a security credential -- HlsHttpOptions::
            # require_playback_token gates real authorization separately, and
            # this production config runs with it off (playback is public).
            # It only distinguishes one viewer from another for the
            # delivery-stats counters HlsHttpHandler and viewer_estimator.py
            # keep, so a request-derived hash is an appropriate source here,
            # not a CSPRNG. If a deployment turns require_playback_token on,
            # this path still only affects viewer counting, never
            # authorization -- the token check itself runs at the origin on
            # every request regardless of how the session id was minted.
            if (req.http.Cookie ~ "rtmp_viewer_id=[0-9a-f]{32}") {
                # A returning viewer's persistent cookie takes precedence,
                # exactly like HlsHttpHandler::handle()'s own cookie-reuse
                # check -- so a viewer polling an undecorated rendition link
                # after the shared cache stripped its Cookie still resolves
                # to the same session instead of minting a new one and
                # looking like a new viewer.
                set req.http.X-Session-Id =
                    regsub(req.http.Cookie, "^.*rtmp_viewer_id=([0-9a-f]{32}).*$", "\1");
                set req.http.X-Session-New = "0";
            } else {
                # Built as separate assignments rather than one
                # req.xid + client.ip + now expression: assigning a single
                # typed value (IP, TIME) to a header is unambiguous VCL, but
                # this avoids depending on how the "+" operator itself
                # coerces mixed-typed operands, which is not exercised
                # anywhere else in this file.
                set req.http.X-Session-Seed = req.xid;
                set req.http.X-Session-Client = client.ip;
                set req.http.X-Session-Seed = req.http.X-Session-Seed + req.http.X-Session-Client;
                set req.http.X-Session-Now = now;
                set req.http.X-Session-Seed = req.http.X-Session-Seed + req.http.X-Session-Now;
                set req.http.X-Session-Id =
                    regsub(digest.hash_sha256(req.http.X-Session-Seed), "^(.{32}).*$", "\1");
                unset req.http.X-Session-Seed;
                unset req.http.X-Session-Client;
                unset req.http.X-Session-Now;
                set req.http.X-Session-New = "1";
            }

            # Strips viewer_session/viewer_stream/viewer_cache the same way
            # HlsHttpHandler::query_without_session_params does, so a forged
            # or duplicate one of these three never survives into the
            # redirect target; anything else in the query (e.g. a playback
            # token, if one is ever enabled) is preserved.
            set req.http.X-Session-Path = regsub(req.url, "\?.*$", "");
            set req.http.X-Session-Query = regsub(req.url, "^[^?]*\??", "");
            set req.http.X-Session-Query =
                regsuball(req.http.X-Session-Query,
                          "(^|&)(viewer_session|viewer_stream|viewer_cache)=[^&]*", "");
            set req.http.X-Session-Query = regsub(req.http.X-Session-Query, "^&", "");
            set req.http.X-Stream-Name = regsub(req.url, "^/hls/[^/]+/([^/]+)/.*$", "\1");

            return (synth(750, "streamforge-session"));
        }
        unset req.http.Cookie;
        unset req.http.Authorization;
        return (hash);
    }
    unset req.http.Cookie;
    unset req.http.Authorization;
    return (hash);
}

sub vcl_synth {
    if (resp.status == 750) {
        # Byte-for-byte the same redirect HlsHttpHandler::handle() builds for
        # this case (hls_http_handler.cpp, the enable_playback_sessions
        # block): a private, uncacheable 302 to the same path with
        # viewer_session/viewer_stream/viewer_cache=1 appended, plus a
        # Set-Cookie only when a session was actually minted (never on
        # cookie reuse -- the value has not changed, so there is nothing to
        # refresh).
        set resp.status = 302;
        set resp.reason = "Found";
        set resp.http.Location = req.http.X-Session-Path + "?";
        if (req.http.X-Session-Query != "") {
            set resp.http.Location = resp.http.Location + req.http.X-Session-Query + "&";
        }
        set resp.http.Location = resp.http.Location
            + "viewer_session=" + req.http.X-Session-Id
            + "&viewer_stream=" + req.http.X-Stream-Name
            + "&viewer_cache=1";
        set resp.http.Content-Type = "text/plain";
        set resp.http.Cache-Control = "private, no-store";
        if (req.http.X-Session-New == "1") {
            # Path/name/Max-Age/Secure mirror HlsHttpOptions' defaults
            # (route_prefix "/hls", viewer_cookie_name "rtmp_viewer_id",
            # viewer_cookie_max_age 24h, viewer_cookie_secure false) exactly
            # as apps/rtmp_server/main.cpp leaves them. If a deployment ever
            # overrides any of those on the origin, update this line to
            # match -- it is not read from the origin's config.
            set resp.http.Set-Cookie = "rtmp_viewer_id=" + req.http.X-Session-Id
                + "; Path=/hls; HttpOnly; SameSite=Lax; Max-Age=86400";
        }
        synthetic("");
        return (deliver);
    }
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
