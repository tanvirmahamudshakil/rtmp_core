#!/usr/bin/env bash
set -Eeuo pipefail

# Production high-fanout acceptance test. Run this from a SEPARATE Linux
# load-generator host so generator CPU and loopback copies are not charged
# to the RTMP server.
#
# Required:
#   PUBLISH_KEY=... PLAYBACK_NAME=... SERVER_HOST=...
#
# Example:
#   SERVER_HOST=10.0.0.10 PUBLISH_KEY=... PLAYBACK_NAME=concert \
#   VIEWERS=1000 DURATION=300 bash scripts/load-test.sh

BUILD_DIR="${BUILD_DIR:-./build/release}"
LOADGEN="${LOADGEN:-${BUILD_DIR}/apps/rtmp_load_gen/rtmp_load_gen}"
SERVER_HOST="${SERVER_HOST:-}"
SERVER_PORT="${SERVER_PORT:-1935}"
APPLICATION="${APPLICATION:-live}"
PUBLISH_KEY="${PUBLISH_KEY:-}"
PLAYBACK_NAME="${PLAYBACK_NAME:-}"
VIEWERS="${VIEWERS:-1000}"
DURATION="${DURATION:-300}"
RAMP_UP_MS="${RAMP_UP_MS:-30000}"
VIDEO_BITRATE="${VIDEO_BITRATE:-2500000}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128000}"
FPS="${FPS:-30}"
KEYFRAME_INTERVAL="${KEYFRAME_INTERVAL:-60}"
SLOW_VIEWERS="${SLOW_VIEWERS:-0}"
ABRUPT_DISCONNECTS="${ABRUPT_DISCONNECTS:-0}"

[[ -n "${SERVER_HOST}" ]] || {
  printf 'SERVER_HOST is required\n' >&2
  exit 2
}
[[ -n "${PUBLISH_KEY}" ]] || {
  printf 'PUBLISH_KEY is required\n' >&2
  exit 2
}
[[ -n "${PLAYBACK_NAME}" ]] || {
  printf 'PLAYBACK_NAME is required\n' >&2
  exit 2
}
[[ -x "${LOADGEN}" ]] || {
  printf 'load generator not executable: %s\n' "${LOADGEN}" >&2
  exit 2
}
[[ "${VIEWERS}" =~ ^[1-9][0-9]*$ ]] || {
  printf 'VIEWERS must be a positive integer\n' >&2
  exit 2
}

exec "${LOADGEN}" \
  --host "${SERVER_HOST}" \
  --port "${SERVER_PORT}" \
  --app "${APPLICATION}" \
  --publish-key "${PUBLISH_KEY}" \
  --playback-name "${PLAYBACK_NAME}" \
  --publishers 1 \
  --viewers "${VIEWERS}" \
  --duration "${DURATION}" \
  --ramp-up "${RAMP_UP_MS}" \
  --video-bitrate "${VIDEO_BITRATE}" \
  --audio-bitrate "${AUDIO_BITRATE}" \
  --fps "${FPS}" \
  --keyframe-interval "${KEYFRAME_INTERVAL}" \
  --slow-viewers "${SLOW_VIEWERS}" \
  --abrupt-disconnects "${ABRUPT_DISCONNECTS}"
