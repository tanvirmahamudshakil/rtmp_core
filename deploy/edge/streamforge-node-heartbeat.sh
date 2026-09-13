#!/usr/bin/env bash
# StreamForge cluster heartbeat.
#
# An edge or shield runs Caddy and Varnish, not rtmp_server, so it cannot put
# itself in the origin's cluster table. This agent does it: every interval it
# reports this node's identity, region and current audience to the origin's
# /api/v1/cluster/nodes endpoint, which is what /v1/cluster/locate places viewers
# from and what the panel lists.
#
# Installed and enabled by install-edge.sh as streamforge-node-heartbeat.timer.
# Run standalone with:
#   STREAMFORGE_MANAGEMENT_URL=https://stream.example.com \
#   STREAMFORGE_NODE_ID=edge-eu-1 STREAMFORGE_NODE_REGION=eu \
#   STREAMFORGE_NODE_ADDRESS=edge1.example.com \
#   bash streamforge-node-heartbeat.sh
#
# Environment:
#   STREAMFORGE_MANAGEMENT_URL  (required) Origin base URL. It may include the
#                               public /api prefix; it is added when omitted.
#   STREAMFORGE_NODE_ID         Defaults to "edge-$(hostname -s)".
#   STREAMFORGE_NODE_ROLE       edge (default) | shield | transcoder.
#   STREAMFORGE_NODE_REGION     Free-form region label used for placement.
#   STREAMFORGE_NODE_ADDRESS    Viewer-facing hostname of this node.
#   STREAMFORGE_NODE_CAPACITY   Viewer ceiling this node was sized for. 0
#                               (default) means unknown: the node is still
#                               selected, but never preferred on load.
#   STREAMFORGE_NODE_DRAINING   "1" to keep serving existing sessions while
#                               taking no new viewers.
#   STREAMFORGE_VIEWER_STATS_PATH
#                               Fresh viewer-estimator JSON used for load.
#   STREAMFORGE_TRANSCODER_AGENT_URL
#                               Local agent URL when role=transcoder.
set -euo pipefail

MANAGEMENT_URL="${STREAMFORGE_MANAGEMENT_URL:-}"
NODE_ID="${STREAMFORGE_NODE_ID:-edge-$(hostname -s 2>/dev/null || hostname)}"
NODE_ROLE="${STREAMFORGE_NODE_ROLE:-edge}"
NODE_REGION="${STREAMFORGE_NODE_REGION:-}"
NODE_ADDRESS="${STREAMFORGE_NODE_ADDRESS:-$(hostname -f 2>/dev/null || hostname)}"
NODE_CAPACITY="${STREAMFORGE_NODE_CAPACITY:-0}"
NODE_DRAINING="${STREAMFORGE_NODE_DRAINING:-0}"
TRANSCODER_AGENT_URL="${STREAMFORGE_TRANSCODER_AGENT_URL:-http://127.0.0.1:9200}"
VIEWER_STATS_PATH="${STREAMFORGE_VIEWER_STATS_PATH:-/var/www/streamforge/internal/viewer_estimate.json}"

if [[ -z "${MANAGEMENT_URL}" ]]; then
  echo "[heartbeat] STREAMFORGE_MANAGEMENT_URL is required" >&2
  exit 2
fi

MANAGEMENT_BASE="${MANAGEMENT_URL%/}"
if [[ "${MANAGEMENT_BASE}" == */api ]]; then
  HEARTBEAT_URL="${MANAGEMENT_BASE}/v1/cluster/nodes"
  FALLBACK_HEARTBEAT_URL=""
else
  # A public origin publishes the control plane below /api. A transcoder or
  # private node may instead point straight at rtmp_server's :8080 listener,
  # where the same route has no prefix; try that legacy/direct shape second.
  HEARTBEAT_URL="${MANAGEMENT_BASE}/api/v1/cluster/nodes"
  FALLBACK_HEARTBEAT_URL="${MANAGEMENT_BASE}/v1/cluster/nodes"
fi

# Current audience on this node. Public clients terminate at Caddy, whose
# keepalive pool multiplexes them over a much smaller number of loopback
# connections to Varnish. Therefore the signal must be viewer_estimator's
# de-duplicated playback sessions from varnishncsa, including cache HITs.
active_viewers() {
  if [[ "${NODE_ROLE}" == "transcoder" ]]; then
    # For a transcoder, the generic cluster capacity/load fields carry job
    # slots and active jobs. Do not report a healthy node when its local
    # agent is unreachable.
    local jobs
    jobs=$(curl -fsS --max-time 5 "${TRANSCODER_AGENT_URL%/}/jobs") || {
      echo "[heartbeat] local transcoder agent is unreachable" >&2
      return 1
    }
    printf '%s' "${jobs}" | awk -F'"id":' '{ count += NF - 1 } END { print count + 0 }'
    return
  fi
  if [[ "${NODE_ROLE}" == "shield" ]]; then
    # A shield has no players of its own. Requests from downstream edges are
    # cache fan-in and must not be reported as viewers.
    echo 0
    return
  fi
  if command -v python3 >/dev/null 2>&1 && [[ -r "${VIEWER_STATS_PATH}" ]]; then
    local estimated
    if estimated=$(python3 - "${VIEWER_STATS_PATH}" <<'PY'
import json
import sys
import time

try:
    with open(sys.argv[1], "r", encoding="utf-8") as source:
        payload = json.load(source)
    generated_at = float(payload["generated_at"])
    window = max(1.0, float(payload.get("window_seconds", 20)))
    viewers = int(payload["totals"]["viewers"])
    # The estimator writes every two seconds. Its viewer window is not a
    # freshness allowance; stop heartbeating well before the registry's 30 s
    # health timeout when the producer has actually died.
    if viewers < 0 or time.time() - generated_at > max(10.0, min(20.0, window)):
        raise ValueError("stale or invalid viewer estimate")
    print(viewers)
except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError):
    raise SystemExit(1)
PY
    ); then
      printf '%s\n' "${estimated}"
      return
    fi
    echo "[heartbeat] viewer estimate is stale or invalid; refusing a misleading heartbeat" >&2
    return 1
  fi
  echo "[heartbeat] viewer estimator output is unavailable at ${VIEWER_STATS_PATH}" >&2
  return 1
}

draining="false"
[[ "${NODE_DRAINING}" == "1" || "${NODE_DRAINING}" == "true" ]] && draining="true"
active=$(active_viewers)

payload=$(cat <<JSON
{"id":"${NODE_ID}","role":"${NODE_ROLE}","address":"${NODE_ADDRESS}","region":"${NODE_REGION}","capacity_viewers":"${NODE_CAPACITY}","active_viewers":"${active}","active_publishers":"0","draining":"${draining}"}
JSON
)

response_file="$(mktemp /tmp/streamforge-heartbeat.XXXXXX)"
trap 'rm -f -- "${response_file}"' EXIT
response_matches_node() {
  # A reverse proxy accidentally serving its HTML fallback also returns HTTP
  # 200. Confirm this was the cluster API and it acknowledged our id.
  if ! command -v python3 >/dev/null 2>&1; then
    grep -Fq "\"id\":\"${NODE_ID}\"" "${response_file}"
    return
  fi
  python3 - "${response_file}" "${NODE_ID}" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as source:
        payload = json.load(source)
    if payload.get("id") != sys.argv[2]:
        raise ValueError("wrong node id")
except (AttributeError, OSError, TypeError, ValueError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

LAST_CODE=000
LAST_ENDPOINT="${HEARTBEAT_URL}"
post_heartbeat() {
  LAST_ENDPOINT="$1"
  LAST_CODE=$(curl -sS -o "${response_file}" -w '%{http_code}' \
    -X POST "${LAST_ENDPOINT}" \
    -H 'Content-Type: application/json' \
    --max-time 10 \
    --data "${payload}") || LAST_CODE=000
  [[ "${LAST_CODE}" == "200" ]] && response_matches_node
}

if post_heartbeat "${HEARTBEAT_URL}"; then
  exit 0
fi
if [[ -n "${FALLBACK_HEARTBEAT_URL}" ]] && post_heartbeat "${FALLBACK_HEARTBEAT_URL}"; then
  exit 0
fi

case "${LAST_CODE}" in
  000) echo "[heartbeat] origin unreachable at ${MANAGEMENT_URL}" >&2 ;;
  200) echo "[heartbeat] ${LAST_ENDPOINT} returned HTTP 200 but not a cluster heartbeat response" >&2 ;;
  *)   echo "[heartbeat] origin returned HTTP ${LAST_CODE}: $(cat "${response_file}")" >&2 ;;
esac
exit 1
