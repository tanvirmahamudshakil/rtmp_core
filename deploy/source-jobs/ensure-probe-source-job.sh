#!/bin/sh
set -eu

api_url="http://127.0.0.1:8080"

# The control listener starts before all service consumers necessarily see it.
# Retry readiness for one minute, then fail the unit loudly instead of leaving
# the public master URL missing without an operational signal.
attempt=0
while ! /usr/bin/curl --max-time 2 --fail --silent "$api_url/health/ready" >/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        echo "rtmp-server control API did not become ready" >&2
        exit 1
    fi
    sleep 2
done

/usr/bin/curl --max-time 15 --fail --silent --show-error \
    -X POST "$api_url/v1/transcoding/source-jobs" \
    -H "X-Application: test" \
    -H "X-Output-Name: probe" \
    -H "X-Source-Url: http://11.fcgool.com:1935/cricket/11.stream/playlist.m3u8" \
    -H "X-Template-Name: probe" \
    -H "X-Auto-Restart: true" \
    -H "X-Restart-Delay-Seconds: 5" \
    --data-binary \
    "test/probe|480|probe_480|default|h264|500000|main|60|854|480|letterbox|aac|96000|first|480"

