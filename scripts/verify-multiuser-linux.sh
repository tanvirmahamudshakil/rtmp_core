#!/usr/bin/env bash
set -Eeuo pipefail

# End-to-end production verification against a running StreamForge instance.
# Requires ffmpeg, curl, and a fresh test application/stream name.
#
# Example:
#   API_TOKEN=... VIEWERS=30 bash scripts/verify-multiuser-linux.sh

API_BASE="${API_BASE:-http://127.0.0.1:8080}"
RTMP_HOST="${RTMP_HOST:-127.0.0.1}"
RTMP_PORT="${RTMP_PORT:-1935}"
TEST_APPLICATION="${TEST_APPLICATION:-codex-load}"
TEST_STREAM="${TEST_STREAM:-multiuser}"
VIEWERS="${VIEWERS:-30}"
VIEW_SECONDS="${VIEW_SECONDS:-10}"
API_TOKEN="${API_TOKEN:-}"

[[ -n "${API_TOKEN}" ]] || {
  printf 'API_TOKEN is required\n' >&2
  exit 2
}
[[ "${VIEWERS}" =~ ^[1-9][0-9]*$ ]] || {
  printf 'VIEWERS must be a positive integer\n' >&2
  exit 2
}
command -v curl >/dev/null
command -v ffmpeg >/dev/null
command -v timeout >/dev/null

work_dir="$(mktemp -d)"
publisher_pid=""
cleanup() {
  if [[ -n "${publisher_pid}" ]]; then
    kill "${publisher_pid}" >/dev/null 2>&1 || true
    wait "${publisher_pid}" >/dev/null 2>&1 || true
  fi
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

auth_header="Authorization: Bearer ${API_TOKEN}"
curl -fsS -H "${auth_header}" -H 'Content-Type: application/json' \
  -d "{\"name\":\"${TEST_APPLICATION}\"}" \
  "${API_BASE}/v1/applications" >/dev/null

stream_response="$(
  curl -fsS -H "${auth_header}" -H 'Content-Type: application/json' \
    -d "{\"application\":\"${TEST_APPLICATION}\",\"name\":\"${TEST_STREAM}\",\"recording_enabled\":false}" \
    "${API_BASE}/v1/streams"
)"
publish_key="$(
  printf '%s' "${stream_response}" |
    sed -nE 's/.*"stream_key"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p'
)"
[[ -n "${publish_key}" ]] || {
  printf 'stream creation did not return a publish key\n' >&2
  exit 1
}

publish_url="rtmp://${RTMP_HOST}:${RTMP_PORT}/${TEST_APPLICATION}/${publish_key}"
playback_url="rtmp://${RTMP_HOST}:${RTMP_PORT}/${TEST_APPLICATION}/${TEST_STREAM}"

timeout "$((VIEW_SECONDS + 12))" ffmpeg -nostdin -hide_banner -loglevel error -re \
  -f lavfi -i 'testsrc2=size=640x360:rate=30' \
  -f lavfi -i 'sine=frequency=1000:sample_rate=48000' \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
  -g 60 -keyint_min 60 -sc_threshold 0 -b:v 1000k -maxrate 1000k -bufsize 2000k \
  -c:a aac -b:a 96k -ar 48000 -f flv "${publish_url}" \
  >"${work_dir}/publisher.log" 2>&1 &
publisher_pid="$!"

# Wait for the publisher to become visible before admitting the audience.
publisher_ready=0
for _ in $(seq 1 50); do
  status="$(
    curl -fsS -H "${auth_header}" \
      "${API_BASE}/v1/streams/${TEST_APPLICATION}:${TEST_STREAM}/status" 2>/dev/null || true
  )"
  if printf '%s' "${status}" | grep -Eq '"is_live"[[:space:]]*:[[:space:]]*true'; then
    publisher_ready=1
    break
  fi
  sleep 0.1
done
[[ "${publisher_ready}" == "1" ]] || {
  printf 'publisher did not become live\n' >&2
  exit 1
}

viewer_pids=()
for viewer in $(seq 1 "${VIEWERS}"); do
  (
    if timeout "$((VIEW_SECONDS + 8))" ffmpeg -nostdin -hide_banner -loglevel error -xerror \
      -i "${playback_url}" -t "${VIEW_SECONDS}" -map 0:v:0 -map 0:a:0 -f null - \
      >"${work_dir}/viewer-${viewer}.log" 2>&1; then
      printf 'ok\n' >"${work_dir}/viewer-${viewer}.status"
    else
      printf 'failed\n' >"${work_dir}/viewer-${viewer}.status"
    fi
  ) &
  viewer_pids+=("$!")
done

sleep 2
live_status="$(
  curl -fsS -H "${auth_header}" \
    "${API_BASE}/v1/streams/${TEST_APPLICATION}:${TEST_STREAM}/status"
)"
peak_viewers="$(
  printf '%s' "${live_status}" |
    sed -nE 's/.*"viewer_count"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p'
)"

for pid in "${viewer_pids[@]}"; do
  wait "${pid}" || true
done

passed=0
failed=0
for viewer in $(seq 1 "${VIEWERS}"); do
  if [[ -f "${work_dir}/viewer-${viewer}.status" ]] &&
     grep -qx 'ok' "${work_dir}/viewer-${viewer}.status"; then
    ((passed += 1))
  else
    ((failed += 1))
  fi
done

printf 'multi-user result: requested=%s decoded=%s failed=%s observed_peak=%s\n' \
  "${VIEWERS}" "${passed}" "${failed}" "${peak_viewers:-unknown}"
if (( failed > 0 || passed != VIEWERS )); then
  for viewer in $(seq 1 "${VIEWERS}"); do
    if [[ -f "${work_dir}/viewer-${viewer}.status" ]] &&
       grep -qx 'failed' "${work_dir}/viewer-${viewer}.status"; then
      printf 'viewer %s error: ' "${viewer}" >&2
      tail -n 1 "${work_dir}/viewer-${viewer}.log" >&2
    fi
  done
  exit 1
fi
