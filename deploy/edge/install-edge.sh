#!/usr/bin/env bash
#
# StreamForge HLS edge / origin-shield installer for a dedicated Debian/Ubuntu
# host. Installs only a caching tier (Varnish + Caddy); no rtmp_server, no
# database. Point one or more of these at a StreamForge origin to serve a much
# larger HLS audience than a single box can, and add a shield in front of the
# origin once you run enough edges that the origin's own Varnish becomes the
# fan-in bottleneck. See docs/multi-node-hls.md.
#
#   Quick start (edge):
#     sudo env \
#       STREAMFORGE_ORIGIN=https://stream.example.com \
#       STREAMFORGE_EDGE_TOKEN=<same value as the origin's hls_edge_fetch_secret> \
#       STREAMFORGE_DOMAIN=edge1.example.com \
#       bash deploy/edge/install-edge.sh
#
#   Quick start (shield, sitting between many edges and the origin):
#     sudo env \
#       STREAMFORGE_ORIGIN=https://stream.example.com \
#       STREAMFORGE_EDGE_TOKEN=<same token> \
#       STREAMFORGE_ROLE=shield \
#       STREAMFORGE_EDGE_CIDRS="203.0.113.0/24,198.51.100.7/32" \
#       bash deploy/edge/install-edge.sh
#
# Inputs:
#   STREAMFORGE_ORIGIN        (required) Origin public HTTPS base URL.
#   STREAMFORGE_EDGE_TOKEN    (required) Shared secret; must equal the origin's
#                             RTMP_SERVER_HLS_EDGE_FETCH_SECRET. >= 16 chars.
#   STREAMFORGE_ROLE          edge (default) | shield. Label + firewall posture.
#   STREAMFORGE_UPSTREAM      Where THIS node fetches from. Defaults to
#                             STREAMFORGE_ORIGIN; set to a shield's URL on an
#                             edge that should pull from the shield instead.
#   STREAMFORGE_DOMAIN        This node's own DNS name for viewer HTTPS. Empty
#                             serves plain HTTP on port 80 (put a CDN/LB in
#                             front, or add the domain later and re-run).
#   STREAMFORGE_EDGE_CIDRS    shield only: comma-separated CIDRs of the edges
#                             allowed to reach the internal Varnish port.
#   STREAMFORGE_EDGE_NODE     Node label surfaced as the X-Edge-Node header.
#                             Default: the hostname.
#   STREAMFORGE_CACHE_SIZE    Varnish malloc store. Default: 75% of RAM,
#                             floor 1g.
#   STREAMFORGE_VARNISH_PORT  Internal Varnish listen port. Default 6081.
#   STREAMFORGE_CADDY_ORIGIN_PORT
#                             Local outbound TLS-proxy port. Default 8090.
#   STREAMFORGE_MANAGEMENT_URL  Origin base URL this node heartbeats to so it
#                             appears in the cluster table and can be handed
#                             viewers by /v1/cluster/locate. Defaults to
#                             STREAMFORGE_ORIGIN; set empty to disable.
#   STREAMFORGE_NODE_REGION   Region label used for placement (e.g. eu, us).
#   STREAMFORGE_NODE_CAPACITY Viewer ceiling this node was sized for. 0
#                             (default) = unknown.
#   STREAMFORGE_EDGE_PROBE    "enabled" (default) health-probes the origin at
#                             /health/ready; "disabled" removes the probe for
#                             an origin that does not expose it publicly.
#   RTMP_CONFIGURE_FIREWALL   1 (default) manages UFW rules when UFW is active.

set -Eeuo pipefail
IFS=$'\n\t'
umask 022

log()  { printf '\033[1;38;5;45m[edge]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[edge] WARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[edge] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[[ ${EUID:-$(id -u)} -eq 0 ]] || die "run as root (sudo)."
command -v apt-get >/dev/null || die "this installer supports Debian/Ubuntu (apt) hosts only."

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." >/dev/null 2>&1 && pwd)"
EDGE_VCL_SRC="${REPO_ROOT}/deploy/varnish/streamforge-edge.vcl"
[[ -f "${EDGE_VCL_SRC}" ]] || die "missing ${EDGE_VCL_SRC}"

ORIGIN="${STREAMFORGE_ORIGIN:-}"
TOKEN="${STREAMFORGE_EDGE_TOKEN:-}"
ROLE="${STREAMFORGE_ROLE:-edge}"
UPSTREAM="${STREAMFORGE_UPSTREAM:-${ORIGIN}}"
DOMAIN="${STREAMFORGE_DOMAIN:-}"
EDGE_CIDRS="${STREAMFORGE_EDGE_CIDRS:-}"
EDGE_NODE="${STREAMFORGE_EDGE_NODE:-$(hostname -s 2>/dev/null || hostname)}"
VARNISH_PORT="${STREAMFORGE_VARNISH_PORT:-6081}"
CADDY_ORIGIN_PORT="${STREAMFORGE_CADDY_ORIGIN_PORT:-8090}"
EDGE_PROBE="${STREAMFORGE_EDGE_PROBE:-enabled}"
MANAGEMENT_URL="${STREAMFORGE_MANAGEMENT_URL-${ORIGIN}}"
NODE_REGION="${STREAMFORGE_NODE_REGION:-}"
NODE_CAPACITY="${STREAMFORGE_NODE_CAPACITY:-0}"
CONFIGURE_FIREWALL="${RTMP_CONFIGURE_FIREWALL:-1}"

[[ -n "${ORIGIN}" ]]  || die "STREAMFORGE_ORIGIN is required (e.g. https://stream.example.com)."
[[ -n "${TOKEN}"  ]]  || die "STREAMFORGE_EDGE_TOKEN is required and must match the origin."
(( ${#TOKEN} >= 16 )) || die "STREAMFORGE_EDGE_TOKEN must be at least 16 characters."
[[ "${ROLE}" == "edge" || "${ROLE}" == "shield" ]] || die "STREAMFORGE_ROLE must be 'edge' or 'shield'."
[[ "${UPSTREAM}" =~ ^https://[^/]+ ]] || die "upstream must be an https:// URL: ${UPSTREAM}"
[[ "${ROLE}" != "shield" || -n "${EDGE_CIDRS}" ]] || \
  warn "shield role with no STREAMFORGE_EDGE_CIDRS: the internal Varnish port will not be firewalled to edges."

UPSTREAM_HOST="${UPSTREAM#https://}"; UPSTREAM_HOST="${UPSTREAM_HOST%%/*}"
UPSTREAM_BARE="${UPSTREAM%/}"

MEM_KB="$(awk '/MemTotal/ {print $2}' /proc/meminfo)"
if [[ -z "${STREAMFORGE_CACHE_SIZE:-}" ]]; then
  CACHE_MB=$(( MEM_KB * 75 / 100 / 1024 ))
  (( CACHE_MB >= 1024 )) || CACHE_MB=1024
  CACHE_SIZE="${CACHE_MB}m"
else
  CACHE_SIZE="${STREAMFORGE_CACHE_SIZE}"
fi

log "role=${ROLE} node=${EDGE_NODE} upstream=${UPSTREAM_BARE} varnish=:${VARNISH_PORT} cache=${CACHE_SIZE}"

log "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends varnish caddy curl ca-certificates >/dev/null

log "Installing edge VCL"
install -D -m 0644 "${EDGE_VCL_SRC}" /etc/varnish/streamforge-edge.vcl
if [[ "${EDGE_PROBE}" == "disabled" ]]; then
  # Replace the probe block with a bare backend so Varnish never marks the
  # origin sick on an endpoint the deployment does not publish.
  sed -i '/\.probe = {/,/^    }/d' /etc/varnish/streamforge-edge.vcl
  log "  health probe disabled per STREAMFORGE_EDGE_PROBE"
fi

log "Configuring Varnish service"
install -d -m 0755 /etc/systemd/system/varnish.service.d
cat > /etc/systemd/system/varnish.service.d/streamforge-edge.conf <<EOF
[Service]
Environment=STREAMFORGE_EDGE_TOKEN=${TOKEN}
Environment=STREAMFORGE_EDGE_ROLE=${ROLE}
Environment=STREAMFORGE_EDGE_NODE=${EDGE_NODE}
ExecStart=
ExecStart=/usr/sbin/varnishd \\
  -a :${VARNISH_PORT} \\
  -f /etc/varnish/streamforge-edge.vcl \\
  -s malloc,${CACHE_SIZE} \\
  -p feature=+http2 \\
  -p thread_pool_min=100 \\
  -p thread_pool_max=5000 \\
  -p workspace_client=256k \\
  -p workspace_backend=256k \\
  -p http_resp_hdr_len=8k \\
  -p http_resp_size=64k \\
  -p pipe_timeout=10s \\
  -F
EOF

log "Configuring Caddy (outbound origin proxy${DOMAIN:+ + viewer TLS for ${DOMAIN}})"
VIEWER_SITE=":80"
[[ -n "${DOMAIN}" ]] && VIEWER_SITE="${DOMAIN}"
cat > /etc/caddy/Caddyfile <<EOF
{
	admin off
}

# Local outbound hop: terminates TLS to the real upstream so Varnish can use
# a plain-HTTP backend and this VCL needs no per-host templating.
http://127.0.0.1:${CADDY_ORIGIN_PORT} {
	reverse_proxy ${UPSTREAM_BARE} {
		header_up Host ${UPSTREAM_HOST}
		transport http {
			tls
			keepalive 90s
		}
	}
}

# Viewer-facing entry. ${DOMAIN:+Automatic HTTPS for ${DOMAIN}.}${DOMAIN:+}
${VIEWER_SITE} {
	encode zstd gzip
	reverse_proxy 127.0.0.1:${VARNISH_PORT}
}
EOF

log "Applying network baselines"
cat > /etc/sysctl.d/99-streamforge-edge.conf <<EOF
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_tw_reuse = 1
net.ipv4.ip_local_port_range = 1024 65535
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
net.ipv4.tcp_slow_start_after_idle = 0
EOF
sysctl --quiet -p /etc/sysctl.d/99-streamforge-edge.conf || warn "some sysctl keys were rejected by this kernel"

if [[ "${CONFIGURE_FIREWALL}" == "1" ]] && command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "Status: active"; then
  log "Updating UFW rules"
  ufw allow 22/tcp   >/dev/null || true
  ufw allow 80/tcp   >/dev/null || true
  ufw allow 443/tcp  >/dev/null || true
  if [[ "${ROLE}" == "shield" && -n "${EDGE_CIDRS}" ]]; then
    IFS=',' read -ra _cidrs <<< "${EDGE_CIDRS}"
    for c in "${_cidrs[@]}"; do
      c="$(echo "$c" | xargs)"
      [[ -n "$c" ]] && ufw allow from "$c" to any port "${VARNISH_PORT}" proto tcp >/dev/null || true
    done
  fi
  ufw --force reload >/dev/null || true
fi

if [[ -n "${MANAGEMENT_URL}" ]]; then
  log "Installing cluster heartbeat (node ${EDGE_NODE}, role ${ROLE})"
  install -m 0755 "$(dirname "$0")/streamforge-node-heartbeat.sh" \
    /usr/local/bin/streamforge-node-heartbeat
  cat > /etc/systemd/system/streamforge-node-heartbeat.service <<EOF
[Unit]
Description=StreamForge cluster heartbeat
After=network-online.target varnish.service

[Service]
Type=oneshot
Environment=STREAMFORGE_MANAGEMENT_URL=${MANAGEMENT_URL}
Environment=STREAMFORGE_NODE_ID=${EDGE_NODE}
Environment=STREAMFORGE_NODE_ROLE=${ROLE}
Environment=STREAMFORGE_NODE_REGION=${NODE_REGION}
Environment=STREAMFORGE_NODE_ADDRESS=$([[ -n "${DOMAIN}" ]] && echo "${DOMAIN}" || echo "${EDGE_NODE}")
Environment=STREAMFORGE_NODE_CAPACITY=${NODE_CAPACITY}
ExecStart=/usr/local/bin/streamforge-node-heartbeat
EOF
  # Every 10 s, comfortably inside the origin's 30 s heartbeat window, so one
  # missed run never marks this node unhealthy.
  cat > /etc/systemd/system/streamforge-node-heartbeat.timer <<EOF
[Unit]
Description=StreamForge cluster heartbeat timer

[Timer]
OnBootSec=15s
OnUnitActiveSec=10s
AccuracySec=1s

[Install]
WantedBy=timers.target
EOF
else
  log "Cluster heartbeat disabled (STREAMFORGE_MANAGEMENT_URL empty)"
fi

log "Starting services"
systemctl daemon-reload
systemctl enable --now caddy >/dev/null
systemctl restart varnish
if [[ -n "${MANAGEMENT_URL}" ]]; then
  systemctl enable --now streamforge-node-heartbeat.timer >/dev/null
fi
systemctl --quiet is-active varnish || die "varnish failed to start -- check: journalctl -u varnish -n 50"

log "Verifying delivery path to the origin"
sleep 2
code="$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: probe.invalid' \
  "http://127.0.0.1:${VARNISH_PORT}/hls/_probe/_probe/index.m3u8" || true)"
case "${code}" in
  404) log "  OK: origin reachable and edge token accepted (404 for a nonexistent stream)." ;;
  403) die "origin returned 403: STREAMFORGE_EDGE_TOKEN does not match the origin's hls_edge_fetch_secret." ;;
  502|503) die "origin unreachable from this node (HTTP ${code}). Check STREAMFORGE_ORIGIN and outbound HTTPS." ;;
  000) die "no response from local Varnish on :${VARNISH_PORT}." ;;
  *)   warn "unexpected probe status ${code}; inspect: varnishlog -g request" ;;
esac

cat <<EOF

$(printf '\033[1;32m[edge] %s node ready\033[0m' "${ROLE}")
  Node label     : ${EDGE_NODE}
  Fetches from   : ${UPSTREAM_BARE}
  Viewer entry   : $([[ -n "${DOMAIN}" ]] && echo "https://${DOMAIN}" || echo "http://<this-host-ip>  (no domain set)")
  Cache store    : ${CACHE_SIZE} (malloc)
  Cluster        : $([[ -n "${MANAGEMENT_URL}" ]] && echo "heartbeating to ${MANAGEMENT_URL} as ${EDGE_NODE}" || echo "not registered (no STREAMFORGE_MANAGEMENT_URL)")
  HLS link shape : <viewer-entry>/hls/<app>/<stream>/index.m3u8

Next:
  * Point viewer DNS for this link at every edge (round-robin A/AAAA, GeoDNS,
    or an L4 load balancer). See docs/multi-node-hls.md.
  * Confirm caching:  curl -sI <viewer-entry>/hls/<app>/<stream>/index.m3u8 | grep -i x-cache
  * Watch traffic:    varnishstat   /   varnishlog -g request
EOF
