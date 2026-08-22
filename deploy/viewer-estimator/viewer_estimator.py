#!/usr/bin/env python3
"""Measure active HLS viewers and delivery traffic at the cache edge.

Every fresh master/index playlist open receives a random ``viewer_session``
from the origin. Session-bearing media-playlist polls are visible on Varnish
cache hits, so one recent session heartbeat equals one active player regardless
of NAT/shared IP. Shared playlist bodies deliberately omit that session from
segment URIs; segment bytes are therefore attributed by their path instead.
Both HIT and MISS body bytes still reflect what the edge actually delivered.

Only aggregate counts are persisted; session IDs never leave process memory.
"""

import json
import os
import re
import selectors
import subprocess
import time
from collections import defaultdict
from urllib.parse import parse_qs, unquote, urlsplit

WINDOW_SECONDS = 20
WRITE_INTERVAL_SECONDS = 2
BITRATE_WINDOW_SECONDS = 10
try:
    MAX_SESSIONS_PER_STREAM = max(
        1, min(10_000_000, int(os.environ.get("RTMP_SERVER_MAXIMUM_VIEWERS_PER_STREAM", "2000000")))
    )
except ValueError:
    MAX_SESSIONS_PER_STREAM = 2_000_000
OUTPUT_PATH = "/var/www/streamforge/internal/viewer_estimate.json"
PATH_RE = re.compile(r"^/hls/([^/]+)/([^/]+)/(index\.m3u8|segment-[^/]+\.ts)$")
SESSION_RE = re.compile(r"^[0-9a-f]{32}$")

# master stream key ("application/stream") -> {session: last_seen_monotonic}
seen = defaultdict(dict)
# Monotonic cache-edge body-byte counters and bounded one-second buckets used
# for the current bitrate. These include HITs, MISSes and byte-range responses.
delivered_bytes = defaultdict(int)
traffic_buckets = defaultdict(dict)


def is_safe_component(value):
    return (
        value not in ("", ".", "..")
        and len(value) <= 256
        and "/" not in value
        and not any(ord(char) < 0x20 or ord(char) == 0x7F for char in value)
    )


def parse_event(raw_line):
    """Return (stream_key, optional_session_id, delivered_bytes), or None."""
    try:
        method, status_text, bytes_text, raw_url = raw_line.split("\t", 3)
        if method != "GET":
            return None
        status = int(status_text)
        if status not in (200, 206):
            return None
        response_bytes = int(bytes_text)
        if response_bytes < 0:
            return None
        parsed = urlsplit(raw_url)
        match = PATH_RE.match(parsed.path)
        if not match:
            return None
        application = unquote(match.group(1))
        if not is_safe_component(application):
            return None
        resource = match.group(3)
        if resource == "index.m3u8":
            params = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=False)
            sessions = params.get("viewer_session", [])
            master_streams = params.get("viewer_stream", [])
            if len(sessions) != 1 or len(master_streams) != 1:
                return None
            session = sessions[0]
            master_stream = master_streams[0]
            if not SESSION_RE.fullmatch(session):
                return None
            # The value was minted from a path component by the origin. Keep
            # the edge parser bounded and reject anything that can alter a key.
            if not is_safe_component(master_stream):
                return None
            return f"{application}/{master_stream}", session, response_bytes

        # Shared playlist bodies contain plain segment URIs. Attribute media
        # bytes to the concrete rendition path; the admin API already sums a
        # source job's declared rendition keys without prefix guessing.
        stream = unquote(match.group(2))
        if not is_safe_component(stream):
            return None
        return f"{application}/{stream}", None, response_bytes
    except (AttributeError, TypeError, ValueError, UnicodeError):
        return None


def prune_and_write(now=None):
    if now is None:
        now = time.monotonic()
    cutoff = now - WINDOW_SECONDS
    counts = {}
    active_sessions = set()
    for key, sessions in list(seen.items()):
        for session, last_seen in list(sessions.items()):
            if last_seen < cutoff:
                del sessions[session]
        if sessions:
            counts[key] = len(sessions)
            active_sessions.update(sessions)
        else:
            del seen[key]

    current_second = int(now)
    bitrate_bps = {}
    aggregate_buckets = defaultdict(int)
    for key, buckets in list(traffic_buckets.items()):
        for second in list(buckets):
            if second <= current_second - BITRATE_WINDOW_SECONDS:
                del buckets[second]
        if buckets:
            for second, response_bytes in buckets.items():
                aggregate_buckets[second] += response_bytes
            oldest = min(buckets)
            # `oldest` is the beginning of its one-second bucket. Measure from
            # that boundary to now; adding a whole extra second systematically
            # understated the live rate, especially for a newly active link.
            elapsed = max(1.0, min(float(BITRATE_WINDOW_SECONDS), now - oldest))
            bitrate_bps[key] = round(sum(buckets.values()) * 8 / elapsed)
        else:
            bitrate_bps[key] = 0

    total_bitrate_bps = 0
    if aggregate_buckets:
        oldest = min(aggregate_buckets)
        elapsed = max(1.0, min(float(BITRATE_WINDOW_SECONDS), now - oldest))
        total_bitrate_bps = round(sum(aggregate_buckets.values()) * 8 / elapsed)

    payload = {
        "generated_at": time.time(),
        "window_seconds": WINDOW_SECONDS,
        "bitrate_window_seconds": BITRATE_WINDOW_SECONDS,
        "identity": "playback_session",
        "viewers": counts,
        "bytes_total": dict(delivered_bytes),
        "bitrate_bps": bitrate_bps,
        # Pre-aggregated totals let consumers avoid summing per-link viewer
        # counts. A playback session can briefly appear under more than one key
        # during an ABR switch or rolling upgrade and must still count once.
        "totals": {
            "viewers": len(active_sessions),
            "bytes_total": sum(delivered_bytes.values()),
            "bitrate_bps": total_bitrate_bps,
        },
    }
    tmp_path = OUTPUT_PATH + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as output:
        json.dump(payload, output, separators=(",", ":"))
    os.chmod(tmp_path, 0o644)
    os.replace(tmp_path, OUTPUT_PATH)


def main():
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    # %q is essential: it retains the per-player session on both HIT and MISS.
    # %b is the response body size delivered by Varnish, so cache hits and
    # partial content are accounted at the public delivery tier.
    proc = subprocess.Popen(
        ["varnishncsa", "-c", "-F", "%m\t%s\t%b\t%U%q"],
        stdout=subprocess.PIPE,
        bufsize=0,
    )
    if proc.stdout is None:
        raise RuntimeError("varnishncsa stdout unavailable")

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    os.set_blocking(proc.stdout.fileno(), False)
    last_write = 0.0
    pending = b""
    try:
        while proc.poll() is None:
            ready = selector.select(timeout=WRITE_INTERVAL_SECONDS)
            now = time.monotonic()
            for key, _ in ready:
                chunk = os.read(key.fileobj.fileno(), 256 * 1024)
                if not chunk:
                    continue
                pending += chunk
                lines = pending.split(b"\n")
                pending = lines.pop()
                for raw_line in lines:
                    event = parse_event(raw_line.decode("utf-8", errors="replace"))
                    if event is None:
                        continue
                    stream_key, session, response_bytes = event
                    if session is not None:
                        sessions = seen[stream_key]
                        # Refresh known viewers even at the cap, but bound
                        # memory under fabricated session query strings.
                        if session in sessions or len(sessions) < MAX_SESSIONS_PER_STREAM:
                            sessions[session] = now
                    delivered_bytes[stream_key] += response_bytes
                    second = int(now)
                    buckets = traffic_buckets[stream_key]
                    buckets[second] = buckets.get(second, 0) + response_bytes
            # Write on a clock, not only when a request arrives. Otherwise the
            # final viewer could remain visible forever after stopping VLC.
            if now - last_write >= WRITE_INTERVAL_SECONDS:
                prune_and_write(now)
                last_write = now
    finally:
        selector.close()
        if proc.poll() is None:
            proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    main()
