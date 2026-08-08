#!/usr/bin/env python3
"""Real HLS viewer estimator.

The origin's own per-IP viewer tracking (hls_http_handler.cpp) only sees
requests that miss the Varnish cache -- with a >95% cache hit ratio that
undercounts real viewers by an order of magnitude. This tails Varnish's
own request log (which sees every request, hit or miss) and reproduces
the same counting rule the origin uses: distinct client IPs that fetched
a rendition's media playlist or a segment within the last 20s (matching
hls_http_handler.hpp's kViewerWindow), keyed by "application/stream" so
it lines up with SourceTranscodeJob outputs / Stream rendition names.

Output is a small JSON file, rewritten every 2s, containing only counts
-- no client IPs are ever persisted to disk.
"""
import json
import os
import re
import subprocess
import time
from collections import defaultdict

WINDOW_SECONDS = 20
OUTPUT_PATH = "/var/lib/rtmp-server/viewer_estimate.json"
PATH_RE = re.compile(r"^/hls/([^/]+)/([^/]+)/(index\.m3u8|segment-[^/]+\.ts)$")

# stream_key ("application/stream") -> {ip: last_seen_monotonic}
seen = defaultdict(dict)
lock_free_last_write = 0.0


def prune_and_write():
    now = time.monotonic()
    cutoff = now - WINDOW_SECONDS
    counts = {}
    for key, ips in list(seen.items()):
        for ip, last in list(ips.items()):
            if last < cutoff:
                del ips[ip]
        if ips:
            counts[key] = len(ips)
        else:
            del seen[key]
    tmp_path = OUTPUT_PATH + ".tmp"
    with open(tmp_path, "w") as f:
        json.dump({"generated_at": time.time(), "window_seconds": WINDOW_SECONDS, "viewers": counts}, f)
    os.replace(tmp_path, OUTPUT_PATH)


def main():
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    # %{X-Forwarded-For}i can repeat (varnish appends its own hop); take the
    # first value in the combined header, which is the real client set by
    # Caddy at the edge. %U is the URL path without query string.
    proc = subprocess.Popen(
        ["varnishncsa", "-F", "%{X-Forwarded-For}i|%U"],
        stdout=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    last_write = time.monotonic()
    for line in proc.stdout:
        line = line.rstrip("\n")
        if "|" not in line:
            continue
        xff, path = line.split("|", 1)
        ip = xff.split(",")[0].strip()
        if not ip:
            continue
        m = PATH_RE.match(path)
        if not m:
            continue
        application, stream = m.group(1), m.group(2)
        seen[f"{application}/{stream}"][ip] = time.monotonic()

        now = time.monotonic()
        if now - last_write >= 2.0:
            prune_and_write()
            last_write = now


if __name__ == "__main__":
    main()
