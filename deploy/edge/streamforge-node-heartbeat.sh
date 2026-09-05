#!/usr/bin/env bash
# StreamForge cluster heartbeat.
#
# An edge or shield runs Caddy and Varnish, not rtmp_server, so it cannot put
# itself in the origin's cluster table. This agent does it: every interval it
# reports this node's identity, region and current audience to the origin's
# /v1/cluster/nodes endpoint, which is what /v1/cluster/locate places viewers
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
#   STREAMFORGE_MANAGEMENT_URL  (required) Origin base URL that serves /v1.
#   STREAMFORGE_NODE_ID         Defaults to "edge-$(hostname -s)".
#   STREAMFORGE_NODE_ROLE       edge (default) | shield | transcoder.
#   STREAMFORGE_NODE_REGION     Free-form region label used for placement.
#   STREAMFORGE_NODE_ADDRESS    Viewer-facing hostname of this node.
#   STREAMFORGE_NODE_CAPACITY   Viewer ceiling this node was sized for. 0
#                               (default) means unknown: the node is still
#                               selected, but never preferred on load.
#   STREAMFORGE_NODE_DRAINING   "1" to keep serving existing sessions while
#                               taking no new viewers.
#   STREAMFORGE_VARNISH_PORT    Local Varnish port, for the viewer count.
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

if [[ -z "${MANAGEMENT_URL}" ]]; then
  echo "[heartbeat] STREAMFORGE_MANAGEMENT_URL is required" >&2
  exit 2
fi

# Current audience on this node. varnishstat's session counter is a total, not
# a gauge, so the live number comes from the sessions Varnish currently holds
# open; a node without varnishstat reports zero rather than guessing.
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
  if ! command -v varnishstat >/dev/null 2>&1; then
    echo 0
    return
  fi
  varnishstat -1 -f MAIN.sess_conn -f MAIN.sess_closed 2>/dev/null |
    awk '
      /MAIN.sess_conn/   { opened = $2 }
      /MAIN.sess_closed/ { closed = $2 }
      END { open_now = opened - closed; print (open_now > 0 ? open_now : 0) }
    ' 2>/dev/null || echo 0
}

draining="false"
[[ "${NODE_DRAINING}" == "1" || "${NODE_DRAINING}" == "true" ]] && draining="true"
active=$(active_viewers)

payload=$(cat <<JSON
{"id":"${NODE_ID}","role":"${NODE_ROLE}","address":"${NODE_ADDRESS}","region":"${NODE_REGION}","capacity_viewers":"${NODE_CAPACITY}","active_viewers":"${active}","active_publishers":"${active}","draining":"${draining}"}
JSON
)

code=$(curl -sS -o /tmp/streamforge-heartbeat.out -w '%{http_code}' \
  -X POST "${MANAGEMENT_URL%/}/v1/cluster/nodes" \
  -H 'Content-Type: application/json' \
  --max-time 10 \
  --data "${payload}" || echo 000)

case "${code}" in
  200) exit 0 ;;
  000) echo "[heartbeat] origin unreachable at ${MANAGEMENT_URL}" >&2; exit 1 ;;
  *)   echo "[heartbeat] origin returned HTTP ${code}: $(cat /tmp/streamforge-heartbeat.out)" >&2; exit 1 ;;
esac
