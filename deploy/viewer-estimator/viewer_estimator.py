#!/usr/bin/env python3
"""Count active HLS playback sessions at the cache edge.

Every fresh master/index playlist open receives a random ``viewer_session``
from the origin. The same value is propagated into variant playlists and
segments. Varnish logs every client request (including cache hits), so one
recent session heartbeat equals one active player regardless of NAT/shared IP.

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
MAX_SESSIONS_PER_STREAM = 50_000
OUTPUT_PATH = "/var/lib/rtmp-server/viewer_estimate.json"
PATH_RE = re.compile(r"^/hls/([^/]+)/([^/]+)/(index\.m3u8|segment-[^/]+\.ts)$")
SESSION_RE = re.compile(r"^[0-9a-f]{32}$")

# master stream key ("application/stream") -> {session: last_seen_monotonic}
seen = defaultdict(dict)


def parse_event(raw_url):
    """Return (master_stream_key, session_id), or None for malformed noise."""
    try:
        parsed = urlsplit(raw_url)
        match = PATH_RE.match(parsed.path)
        if not match:
            return None
        params = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=False)
        sessions = params.get("viewer_session", [])
        master_streams = params.get("viewer_stream", [])
        if len(sessions) != 1 or len(master_streams) != 1:
            return None
        session = sessions[0]
        master_stream = master_streams[0]
        if not SESSION_RE.fullmatch(session):
            return None
        # The value was minted from a path component by the origin. Keep the
        # edge parser bounded and reject anything that could alter our key.
        if not master_stream or len(master_stream) > 256 or "/" in master_stream or any(
            ord(char) < 0x20 or ord(char) == 0x7F for char in master_stream
        ):
            return None
        application = unquote(match.group(1))
        if not application or len(application) > 256 or "/" in application:
            return None
        return f"{application}/{master_stream}", session
    except (TypeError, ValueError, UnicodeError):
        return None


def prune_and_write(now=None):
    if now is None:
        now = time.monotonic()
    cutoff = now - WINDOW_SECONDS
    counts = {}
    for key, sessions in list(seen.items()):
        for session, last_seen in list(sessions.items()):
            if last_seen < cutoff:
                del sessions[session]
        if sessions:
            counts[key] = len(sessions)
        else:
            del seen[key]

    payload = {
        "generated_at": time.time(),
        "window_seconds": WINDOW_SECONDS,
        "identity": "playback_session",
        "viewers": counts,
    }
    tmp_path = OUTPUT_PATH + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as output:
        json.dump(payload, output, separators=(",", ":"))
    os.replace(tmp_path, OUTPUT_PATH)


def main():
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    # %q is essential: it retains the per-player session on both HIT and MISS.
    proc = subprocess.Popen(
        ["varnishncsa", "-c", "-F", "%U%q"],
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
                    stream_key, session = event
                    sessions = seen[stream_key]
                    # Refresh known viewers even at the cap, but bound memory
                    # under a flood of fabricated session query strings.
                    if session in sessions or len(sessions) < MAX_SESSIONS_PER_STREAM:
                        sessions[session] = now
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
