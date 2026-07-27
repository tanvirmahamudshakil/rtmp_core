#!/usr/bin/env bash
#
# StreamForge one-command installer for a dedicated Debian/Ubuntu VPS.
#
# Quick start:
#   sudo env RTMP_DOMAIN=stream.example.com RTMP_BANDWIDTH_MBIT=auto \
#     bash scripts/install-linux.sh
#
# Supported inputs:
#   RTMP_DOMAIN                  DNS name for HTTPS panel and RTMP URLs.
#                                Empty = HTTP on the server's primary IP.
#   RTMP_BANDWIDTH_MBIT          "auto" (default) reads the NIC-reported link
#                                speed; set Mbps explicitly for provider caps.
#   RTMP_EXPECTED_STREAM_MBIT    "auto" (default) keeps OBS/transcoder bitrate
#                                unchanged and measures it while live. A numeric
#                                value only overrides install-time capacity sizing.
#   RTMP_RESOURCE_SIZING_MBIT    Pre-live socket/buffer sizing floor used only
#                                in auto mode (default 0.50 Mbps). It never
#                                changes or transcodes media.
#   RTMP_LINK_UTILIZATION_PERCENT
#                                Capacity target (default 90, accepted 50-95).
#   RTMP_PROTOCOL_OVERHEAD_PERCENT
#                                RTMP/TCP/IP overhead budget (default 5).
#   RTMP_ADMIN_TOKEN             Existing admin token; generated if absent.
#   RTMP_ENABLE_FAIR_QUEUE       1 (default) enables CAKE up to 10 Gbps and
#                                high-throughput fq above it.
#   RTMP_CONFIGURE_FIREWALL      1 (default) adds rules only if UFW is active.
#   RTMP_WORKERS                 Worker count; default min(nproc, 16).
#   RTMP_MAX_CONNECTIONS_PER_IP  Per-source safety limit (default 1000).
#   RTMP_FORCE_ROTATE_SECRETS    1 rotates secrets during an existing install.
#
set -Eeuo pipefail
IFS=$'\n\t'
umask 027

log() { printf '\033[1;38;5;214m[streamforge]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[streamforge] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  die "Run this installer as root (for example: sudo env RTMP_DOMAIN=... bash scripts/install-linux.sh)."
fi
[[ $(uname -s) == "Linux" ]] || die "The production transport requires Linux io_uring."
command -v systemctl >/dev/null 2>&1 || die "systemd is required."

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
[[ -f "${SOURCE_DIR}/CMakeLists.txt" && -f "${SOURCE_DIR}/admin/package.json" ]] ||
  die "Run the installer from a complete StreamForge source checkout."

if [[ -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
else
  die "Cannot identify the Linux distribution."
fi
case "${ID:-}" in
  ubuntu)
    dpkg --compare-versions "${VERSION_ID:-0}" ge 24.04 ||
      die "Ubuntu 24.04+ is required for the C++23 production toolchain."
    ;;
  debian)
    dpkg --compare-versions "${VERSION_ID:-0}" ge 13 ||
      die "Debian 13+ is required for the C++23 production toolchain."
    ;;
  *) die "Supported distributions: Ubuntu 24.04+ and Debian 13+ (found ${ID:-unknown})." ;;
esac

DOMAIN="${RTMP_DOMAIN:-}"
BANDWIDTH_MBIT="${RTMP_BANDWIDTH_MBIT:-auto}"
EXPECTED_STREAM_MBIT="${RTMP_EXPECTED_STREAM_MBIT:-auto}"
RESOURCE_SIZING_MBIT="${RTMP_RESOURCE_SIZING_MBIT:-0.50}"
LINK_UTILIZATION_PERCENT="${RTMP_LINK_UTILIZATION_PERCENT:-90}"
PROTOCOL_OVERHEAD_PERCENT="${RTMP_PROTOCOL_OVERHEAD_PERCENT:-5}"
MAX_CONNECTIONS_PER_IP="${RTMP_MAX_CONNECTIONS_PER_IP:-1000}"
ENABLE_FAIR_QUEUE="${RTMP_ENABLE_FAIR_QUEUE:-1}"
CONFIGURE_FIREWALL="${RTMP_CONFIGURE_FIREWALL:-1}"
FORCE_ROTATE="${RTMP_FORCE_ROTATE_SECRETS:-0}"

if [[ "${BANDWIDTH_MBIT}" != "auto" ]]; then
  [[ "${BANDWIDTH_MBIT}" =~ ^[1-9][0-9]*$ ]] ||
    die "RTMP_BANDWIDTH_MBIT must be 'auto' or a whole number in Mbps."
  (( BANDWIDTH_MBIT <= 1000000 )) || die "RTMP_BANDWIDTH_MBIT is outside the supported range."
  (( BANDWIDTH_MBIT >= 10 )) || die "RTMP_BANDWIDTH_MBIT must be at least 10 Mbps."
fi
if [[ "${EXPECTED_STREAM_MBIT}" != "auto" ]]; then
  [[ "${EXPECTED_STREAM_MBIT}" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
    die "RTMP_EXPECTED_STREAM_MBIT must be 'auto' or numeric Mbps."
fi
[[ "${RESOURCE_SIZING_MBIT}" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
  die "RTMP_RESOURCE_SIZING_MBIT must be numeric."
[[ "${LINK_UTILIZATION_PERCENT}" =~ ^[0-9]+$ ]] ||
  die "RTMP_LINK_UTILIZATION_PERCENT must be a whole percentage."
(( LINK_UTILIZATION_PERCENT >= 50 && LINK_UTILIZATION_PERCENT <= 95 )) ||
  die "RTMP_LINK_UTILIZATION_PERCENT must be between 50 and 95."
[[ "${PROTOCOL_OVERHEAD_PERCENT}" =~ ^[0-9]+$ ]] ||
  die "RTMP_PROTOCOL_OVERHEAD_PERCENT must be a whole percentage."
(( PROTOCOL_OVERHEAD_PERCENT >= 1 && PROTOCOL_OVERHEAD_PERCENT <= 20 )) ||
  die "RTMP_PROTOCOL_OVERHEAD_PERCENT must be between 1 and 20."
[[ "${MAX_CONNECTIONS_PER_IP}" =~ ^[1-9][0-9]*$ ]] ||
  die "RTMP_MAX_CONNECTIONS_PER_IP must be a positive integer."
(( MAX_CONNECTIONS_PER_IP <= 1000000 )) ||
  die "RTMP_MAX_CONNECTIONS_PER_IP is outside the supported range."
[[ "${ENABLE_FAIR_QUEUE}" =~ ^[01]$ ]] || die "RTMP_ENABLE_FAIR_QUEUE must be 0 or 1."
[[ "${CONFIGURE_FIREWALL}" =~ ^[01]$ ]] || die "RTMP_CONFIGURE_FIREWALL must be 0 or 1."
[[ "${FORCE_ROTATE}" =~ ^[01]$ ]] || die "RTMP_FORCE_ROTATE_SECRETS must be 0 or 1."
if [[ -n "${DOMAIN}" && ! "${DOMAIN}" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]]; then
  die "RTMP_DOMAIN must be a hostname without a URL scheme, path, port or whitespace."
fi
if [[ "${EXPECTED_STREAM_MBIT}" != "auto" ]]; then
  awk -v value="${EXPECTED_STREAM_MBIT}" 'BEGIN { exit !(value >= 0.05 && value <= 100) }' ||
    die "RTMP_EXPECTED_STREAM_MBIT must be between 0.05 and 100 Mbps."
fi
awk -v value="${RESOURCE_SIZING_MBIT}" 'BEGIN { exit !(value >= 0.05 && value <= 100) }' ||
  die "RTMP_RESOURCE_SIZING_MBIT must be between 0.05 and 100 Mbps."

BITRATE_MODE="auto"
CAPACITY_STREAM_MBIT="${RESOURCE_SIZING_MBIT}"
EXPECTED_STREAM_JSON="null"
if [[ "${EXPECTED_STREAM_MBIT}" != "auto" ]]; then
  BITRATE_MODE="fixed-capacity-override"
  CAPACITY_STREAM_MBIT="${EXPECTED_STREAM_MBIT}"
  EXPECTED_STREAM_JSON="${EXPECTED_STREAM_MBIT}"
fi

log "Installing compiler, runtime, web server and network tooling"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential clang cmake ninja-build pkg-config git ca-certificates curl \
  liburing-dev liburing2 libssl-dev libsqlite3-dev libsqlite3-0 sqlite3 \
  nodejs npm iproute2 ethtool kmod
if ! apt-cache show caddy >/dev/null 2>&1 && [[ "${ID}" == "ubuntu" ]]; then
  apt-get install -y --no-install-recommends software-properties-common
  add-apt-repository -y universe
  apt-get update
fi
apt-get install -y --no-install-recommends caddy ||
  die "Caddy is unavailable from this distribution's configured repositories."

CMAKE_VERSION="$(cmake --version | awk 'NR == 1 { print $3 }')"
dpkg --compare-versions "${CMAKE_VERSION}" ge 3.25 ||
  die "CMake 3.25+ is required; this distribution provides ${CMAKE_VERSION}."

CPU_COUNT="$(nproc)"
DEFAULT_WORKERS="${CPU_COUNT}"
if (( DEFAULT_WORKERS > 16 )); then DEFAULT_WORKERS=16; fi
WORKERS="${RTMP_WORKERS:-${DEFAULT_WORKERS}}"
[[ "${WORKERS}" =~ ^[1-9][0-9]*$ ]] || die "RTMP_WORKERS must be a positive integer."
if (( WORKERS > CPU_COUNT )); then WORKERS="${CPU_COUNT}"; fi
if (( WORKERS > 64 )); then WORKERS=64; fi

PRIMARY_INTERFACE="$(ip -o route get 1.1.1.1 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}')"
[[ -n "${PRIMARY_INTERFACE}" ]] || PRIMARY_INTERFACE="$(ip -o route show default | awk 'NR == 1 { print $5 }')"
[[ -n "${PRIMARY_INTERFACE}" ]] || die "Could not detect the primary network interface."

PRIMARY_IP="$(ip -o -4 addr show dev "${PRIMARY_INTERFACE}" scope global | awk 'NR == 1 { split($4,a,"/"); print a[1] }')"
[[ -n "${PRIMARY_IP}" ]] || PRIMARY_IP="$(hostname -I | awk '{ print $1 }')"
[[ -n "${PRIMARY_IP}" ]] || die "Could not detect this server's primary IPv4 address."
PUBLIC_HOST="${DOMAIN:-${PRIMARY_IP}}"

BANDWIDTH_SOURCE="operator-provided committed rate"
BANDWIDTH_SOURCE_KIND="operator"
if [[ "${BANDWIDTH_MBIT}" == "auto" ]]; then
  DETECTED_BANDWIDTH=""
  if [[ -r "/sys/class/net/${PRIMARY_INTERFACE}/speed" ]]; then
    read -r DETECTED_BANDWIDTH < "/sys/class/net/${PRIMARY_INTERFACE}/speed" || true
  fi
  if [[ ! "${DETECTED_BANDWIDTH}" =~ ^[1-9][0-9]*$ ]]; then
    DETECTED_BANDWIDTH="$(ethtool "${PRIMARY_INTERFACE}" 2>/dev/null |
      awk -F: '/^[[:space:]]*Speed:/ {
        value=$2; gsub(/[[:space:]]|Mb\/s/, "", value);
        if (value ~ /^[0-9]+$/) print value;
        exit
      }')"
  fi
  [[ "${DETECTED_BANDWIDTH}" =~ ^[1-9][0-9]*$ ]] ||
    die "NIC ${PRIMARY_INTERFACE} does not report link speed; rerun with RTMP_BANDWIDTH_MBIT=<provider Mbps>."
  (( DETECTED_BANDWIDTH >= 10 && DETECTED_BANDWIDTH <= 1000000 )) ||
    die "NIC-reported speed ${DETECTED_BANDWIDTH} Mbps is outside the supported range."
  BANDWIDTH_MBIT="${DETECTED_BANDWIDTH}"
  BANDWIDTH_SOURCE="NIC-reported link speed (verify against the provider plan)"
  BANDWIDTH_SOURCE_KIND="nic"
  log "Auto-detected ${BANDWIDTH_MBIT} Mbps on ${PRIMARY_INTERFACE}; provider shaping may be lower"
fi

MAX_VIEWERS="$(awk -v bw="${BANDWIDTH_MBIT}" -v rate="${CAPACITY_STREAM_MBIT}" \
  -v utilization="${LINK_UTILIZATION_PERCENT}" -v overhead="${PROTOCOL_OVERHEAD_PERCENT}" \
  'BEGIN {
    value=int((bw * (utilization / 100.0)) / (rate * (1.0 + overhead / 100.0)));
    if (value < 1) value=1;
    print value
  }')"
CONNECTION_RESERVE=$((MAX_VIEWERS / 10))
if (( CONNECTION_RESERVE < 2048 )); then CONNECTION_RESERVE=2048; fi
MAX_CONNECTIONS=$((MAX_VIEWERS + CONNECTION_RESERVE))
(( MAX_CONNECTIONS <= 4000000000 )) ||
  die "Calculated connection budget exceeds the single-process configuration range."
MAX_PUBLISHERS=$((MAX_CONNECTIONS / 10))
if (( MAX_PUBLISHERS < 1000 )); then MAX_PUBLISHERS=1000; fi
if (( MAX_PUBLISHERS > 10000 )); then MAX_PUBLISHERS=10000; fi
PER_WORKER_CONNECTIONS=$(((MAX_CONNECTIONS + WORKERS - 1) / WORKERS))
BUFFER_RESERVE=$((PER_WORKER_CONNECTIONS / 10))
if (( BUFFER_RESERVE < 256 )); then BUFFER_RESERVE=256; fi
PROVIDED_BUFFER_COUNT=$((PER_WORKER_CONNECTIONS + BUFFER_RESERVE))
NOFILE=$((MAX_CONNECTIONS + 65536))
if (( NOFILE < 65536 )); then NOFILE=65536; fi
if (( NOFILE > 1048576 )); then NOFILE=1048576; fi
(( MAX_CONNECTIONS < NOFILE )) ||
  die "Calculated ${MAX_CONNECTIONS} sockets exceed this single-node file-descriptor ceiling; use multiple origin nodes."

log "Building hardened C++ server with ${WORKERS} media workers"
export CC=clang
export CXX=clang++
cmake --preset production
cmake --build --preset production --parallel "${CPU_COUNT}"
SERVER_BINARY="${SOURCE_DIR}/build/production/apps/rtmp_server/rtmp_server"
[[ -x "${SERVER_BINARY}" ]] || die "Production server binary was not produced."

log "Building pinned admin panel assets"
(
  cd "${SOURCE_DIR}/admin"
  npm ci --no-audit --no-fund
  npm run build
)
[[ -f "${SOURCE_DIR}/admin/dist/index.html" ]] || die "Admin panel build did not produce index.html."

if ! getent group rtmp-server >/dev/null; then
  groupadd --system rtmp-server
fi
if ! id rtmp-server >/dev/null 2>&1; then
  useradd --system --gid rtmp-server --no-create-home --home-dir /var/lib/rtmp-server \
    --shell /usr/sbin/nologin rtmp-server
fi

install -d -m 0750 -o rtmp-server -g rtmp-server \
  /var/lib/rtmp-server /var/lib/rtmp-server/recordings
install -d -m 0750 -o root -g rtmp-server /etc/rtmp-server
install -d -m 0755 -o root -g root /var/www/streamforge /usr/share/doc/rtmp-server

if [[ -x /usr/local/bin/rtmp-server ]]; then
  cp -a /usr/local/bin/rtmp-server /usr/local/bin/rtmp-server.previous
fi
install -m 0755 -o root -g root "${SERVER_BINARY}" /usr/local/bin/rtmp-server
install -m 0640 -o root -g rtmp-server "${SOURCE_DIR}/config/server.example.yaml" /etc/rtmp-server/server.yaml
install -m 0644 -o root -g root "${SOURCE_DIR}/docs/deployment.md" /usr/share/doc/rtmp-server/deployment.md
cp -a "${SOURCE_DIR}/admin/dist/." /var/www/streamforge/
cat > /var/www/streamforge/runtime-config.json <<EOF
{
  "bandwidth_mbps": ${BANDWIDTH_MBIT},
  "bandwidth_source": "${BANDWIDTH_SOURCE_KIND}",
  "bitrate_mode": "${BITRATE_MODE}",
  "expected_stream_mbps": ${EXPECTED_STREAM_JSON},
  "resource_sizing_stream_mbps": ${CAPACITY_STREAM_MBIT},
  "utilization_percent": ${LINK_UTILIZATION_PERCENT},
  "protocol_overhead_percent": ${PROTOCOL_OVERHEAD_PERCENT},
  "viewer_budget": ${MAX_VIEWERS}
}
EOF
chown -R root:root /var/www/streamforge
find /var/www/streamforge -type d -exec chmod 0755 {} +
find /var/www/streamforge -type f -exec chmod 0644 {} +

EXISTING_ENV=/etc/rtmp-server/rtmp-server.env
read_existing_secret() {
  local key="$1"
  if [[ -r "${EXISTING_ENV}" ]]; then
    sed -n "s/^${key}=//p" "${EXISTING_ENV}" | tail -n 1
  fi
}

ADMIN_TOKEN="${RTMP_ADMIN_TOKEN:-}"
TOKEN_SECRET=""
if [[ "${FORCE_ROTATE}" != "1" ]]; then
  [[ -n "${ADMIN_TOKEN}" ]] || ADMIN_TOKEN="$(read_existing_secret RTMP_SERVER_API_AUTHENTICATION_SECRET)"
  TOKEN_SECRET="$(read_existing_secret RTMP_SERVER_TOKEN_SIGNING_SECRET)"
fi
[[ -n "${ADMIN_TOKEN}" ]] || ADMIN_TOKEN="$(openssl rand -hex 32)"
[[ -n "${TOKEN_SECRET}" ]] || TOKEN_SECRET="$(openssl rand -hex 32)"
[[ "${ADMIN_TOKEN}" != "${TOKEN_SECRET}" ]] || TOKEN_SECRET="$(openssl rand -hex 32)"
[[ "${ADMIN_TOKEN}" =~ ^[A-Za-z0-9._~-]{32,256}$ ]] ||
  die "RTMP_ADMIN_TOKEN must be 32-256 URL-safe characters."
[[ "${TOKEN_SECRET}" =~ ^[A-Za-z0-9._~-]{32,256}$ ]] ||
  die "The existing token-signing secret is invalid; set RTMP_FORCE_ROTATE_SECRETS=1."

cat > "${EXISTING_ENV}" <<EOF
RTMP_SERVER_TOKEN_SIGNING_SECRET=${TOKEN_SECRET}
RTMP_SERVER_API_AUTHENTICATION_SECRET=${ADMIN_TOKEN}
RTMP_SERVER_RTMP_BIND_ADDRESS=0.0.0.0
RTMP_SERVER_RTMP_PORT=1935
RTMP_SERVER_API_BIND_ADDRESS=127.0.0.1
RTMP_SERVER_API_PORT=8080
RTMP_SERVER_PUBLIC_RTMP_HOSTNAME=${PUBLIC_HOST}
RTMP_SERVER_MAXIMUM_CONNECTIONS=${MAX_CONNECTIONS}
RTMP_SERVER_MAXIMUM_CONNECTIONS_PER_IP=${MAX_CONNECTIONS_PER_IP}
RTMP_SERVER_MAXIMUM_PUBLISHERS=${MAX_PUBLISHERS}
RTMP_SERVER_MAXIMUM_VIEWERS_PER_STREAM=${MAX_VIEWERS}
RTMP_SERVER_WORKER_RING_COUNT=${WORKERS}
RTMP_SERVER_PROVIDED_BUFFER_COUNT=${PROVIDED_BUFFER_COUNT}
RTMP_SERVER_PROVIDED_BUFFER_SIZE=16384
RTMP_SERVER_DATABASE_TYPE=sqlite
RTMP_SERVER_DATABASE_CONNECTION=/var/lib/rtmp-server/rtmp.db
RTMP_SERVER_RECORDING_ENABLED=false
RTMP_SERVER_RECORDING_DIRECTORY=/var/lib/rtmp-server/recordings
RTMP_SERVER_LOG_LEVEL=info
RTMP_SERVER_METRICS_ENABLED=true
EOF
chown root:rtmp-server "${EXISTING_ENV}"
chmod 0640 "${EXISTING_ENV}"

CREDENTIALS=/root/streamforge-credentials.txt
cat > "${CREDENTIALS}" <<EOF
StreamForge Control
===================
Admin URL: $(if [[ -n "${DOMAIN}" ]]; then printf 'https://%s' "${DOMAIN}"; else printf 'http://%s' "${PRIMARY_IP}"; fi)
RTMP origin: rtmp://${PUBLIC_HOST}:1935
Admin token: ${ADMIN_TOKEN}
Capacity bandwidth: ${BANDWIDTH_MBIT} Mbps (${BANDWIDTH_SOURCE})
Bitrate mode: $(if [[ "${BITRATE_MODE}" == "auto" ]]; then printf 'OBS/transcoder passthrough, measured while live'; else printf 'manual capacity override (%s Mbps)' "${EXPECTED_STREAM_MBIT}"; fi)
Pre-live viewer safety ceiling: ${MAX_VIEWERS}
Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)

Keep this file private. The admin token controls stream creation and key rotation.
EOF
chmod 0600 "${CREDENTIALS}"

log "Installing hardened systemd service"
cat > /etc/systemd/system/rtmp-server.service <<EOF
[Unit]
Description=StreamForge C++23 io_uring RTMP server
Documentation=file:///usr/share/doc/rtmp-server/deployment.md
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=60s
StartLimitBurst=5

[Service]
Type=simple
ExecStart=/usr/local/bin/rtmp-server --config /etc/rtmp-server/server.yaml
EnvironmentFile=/etc/rtmp-server/rtmp-server.env
User=rtmp-server
Group=rtmp-server
KillSignal=SIGTERM
KillMode=mixed
TimeoutStopSec=30s
Restart=always
RestartSec=2s
RestartPreventExitStatus=1
LimitNOFILE=${NOFILE}
LimitMEMLOCK=infinity
LimitCORE=0
OOMScoreAdjust=200
OOMPolicy=stop
StateDirectory=rtmp-server
ConfigurationDirectory=rtmp-server
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/rtmp-server
PrivateTmp=true
PrivateDevices=true
ProtectProc=invisible
ProcSubset=pid
ProtectClock=true
ProtectHostname=true
ProtectKernelLogs=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
NoNewPrivileges=true
RestrictSUIDSGID=true
RestrictRealtime=true
RestrictNamespaces=true
RemoveIPC=true
LockPersonality=true
MemoryDenyWriteExecute=true
SystemCallArchitectures=native
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
CapabilityBoundingSet=
AmbientCapabilities=
SystemCallFilter=@system-service
SystemCallFilter=io_uring_setup io_uring_enter io_uring_register
SystemCallErrorNumber=EPERM
StandardOutput=journal
StandardError=journal
SyslogIdentifier=rtmp-server

[Install]
WantedBy=multi-user.target
EOF

log "Applying measured-workload Linux network baselines"
cat > /etc/sysctl.d/60-streamforge.conf <<'EOF'
fs.file-max = 2097152
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 65536
net.ipv4.tcp_syncookies = 1
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
net.core.default_qdisc = fq
EOF
if modprobe tcp_bbr 2>/dev/null && sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null | grep -qw bbr; then
  printf '%s\n' 'net.ipv4.tcp_congestion_control = bbr' >> /etc/sysctl.d/60-streamforge.conf
fi
sysctl --system >/dev/null

log "Configuring NIC ${PRIMARY_INTERFACE}"
ethtool -K "${PRIMARY_INTERFACE}" gro on gso on tso on >/dev/null 2>&1 || true
if [[ "${ENABLE_FAIR_QUEUE}" == "1" ]]; then
  SHAPE_MBIT=$((BANDWIDTH_MBIT * 95 / 100))
  cat > /etc/default/rtmp-network <<EOF
RTMP_INTERFACE=${PRIMARY_INTERFACE}
RTMP_SHAPE_MBIT=${SHAPE_MBIT}
EOF
  cat > /usr/local/sbin/rtmp-network-tune <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. /etc/default/rtmp-network
[[ -n "${RTMP_INTERFACE:-}" && "${RTMP_SHAPE_MBIT:-0}" =~ ^[0-9]+$ ]] || exit 1
if (( RTMP_SHAPE_MBIT <= 10000 )) && modprobe sch_cake 2>/dev/null; then
  tc qdisc replace dev "${RTMP_INTERFACE}" root cake bandwidth "${RTMP_SHAPE_MBIT}Mbit" \
    besteffort dual-dsthost nat nowash
else
  # CAKE becomes CPU-expensive at 10G+ line rates. Linux fq preserves
  # per-flow pacing/fairness with materially less CPU overhead.
  tc qdisc replace dev "${RTMP_INTERFACE}" root fq limit 100000 flow_limit 1000 buckets 65536 ||
    tc qdisc replace dev "${RTMP_INTERFACE}" root fq
fi
EOF
  chmod 0755 /usr/local/sbin/rtmp-network-tune
  cat > /etc/systemd/system/rtmp-network-tune.service <<'EOF'
[Unit]
Description=StreamForge fair egress queue
After=network-online.target
Wants=network-online.target
Before=rtmp-server.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/rtmp-network-tune
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
  systemctl enable rtmp-network-tune.service >/dev/null
else
  systemctl disable --now rtmp-network-tune.service >/dev/null 2>&1 || true
  # An idempotent rerun with fair queueing disabled must not leave the
  # previous installer-managed CAKE qdisc active at an outdated link rate.
  if [[ -f /etc/default/rtmp-network ]]; then
    tc qdisc replace dev "${PRIMARY_INTERFACE}" root fq >/dev/null 2>&1 || true
  fi
fi

log "Configuring Caddy admin endpoint"
if [[ -n "${DOMAIN}" ]]; then
  CADDY_SITE="${DOMAIN}"
else
  CADDY_SITE=":80"
fi
cat > /etc/caddy/Caddyfile <<EOF
${CADDY_SITE} {
    encode zstd gzip

    @control path /api/*
    handle @control {
        uri strip_prefix /api
        reverse_proxy 127.0.0.1:8080
    }

    handle {
        root * /var/www/streamforge
        try_files {path} /index.html
        file_server
    }

    header {
        -Server
        X-Content-Type-Options nosniff
        X-Frame-Options DENY
        Referrer-Policy no-referrer
        Permissions-Policy "camera=(), microphone=(), geolocation=()"
        Content-Security-Policy "default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'"
    }
}
EOF
caddy validate --config /etc/caddy/Caddyfile

if command -v ufw >/dev/null 2>&1 && [[ "${CONFIGURE_FIREWALL}" == "1" ]] &&
   ufw status | grep -q '^Status: active'; then
  log "Adding ports to the active UFW policy"
  ufw allow 1935/tcp comment 'StreamForge RTMP'
  ufw allow 80/tcp comment 'StreamForge HTTP'
  ufw allow 443/tcp comment 'StreamForge HTTPS'
fi

systemctl daemon-reload
if [[ "${ENABLE_FAIR_QUEUE}" == "1" ]]; then
  systemctl restart rtmp-network-tune.service
fi
systemctl enable --now caddy.service >/dev/null
systemctl restart caddy.service
systemctl enable rtmp-server.service >/dev/null
systemctl restart rtmp-server.service

log "Waiting for the management readiness check"
READY=0
for _ in $(seq 1 30); do
  if curl -fsS http://127.0.0.1:8080/health/ready >/dev/null; then
    READY=1
    break
  fi
  sleep 1
done
if [[ "${READY}" != "1" ]]; then
  journalctl -u rtmp-server.service -n 80 --no-pager >&2 || true
  die "The service did not become ready. Review the journal output above."
fi

ADMIN_URL="http://${PRIMARY_IP}"
if [[ -n "${DOMAIN}" ]]; then ADMIN_URL="https://${DOMAIN}"; fi

printf '\n\033[1;32mStreamForge installation complete.\033[0m\n'
printf '  Admin panel:      %s\n' "${ADMIN_URL}"
printf '  RTMP origin:      rtmp://%s:1935\n' "${PUBLIC_HOST}"
printf '  Network device:   %s\n' "${PRIMARY_INTERFACE}"
printf '  Link bandwidth:   %s Mbps — %s\n' "${BANDWIDTH_MBIT}" "${BANDWIDTH_SOURCE}"
printf '  Media workers:    %s\n' "${WORKERS}"
if [[ "${BITRATE_MODE}" == "auto" ]]; then
  printf '  Bitrate source:   OBS/transcoder traffic, measured after publishing starts\n'
  printf '  Safety ceiling:   %s viewers (resources sized at %s Mbps; media is not altered)\n' \
    "${MAX_VIEWERS}" "${CAPACITY_STREAM_MBIT}"
else
  printf '  Capacity ceiling: %s viewers at %s Mbps (%s%% link, %s%% overhead)\n' \
    "${MAX_VIEWERS}" "${CAPACITY_STREAM_MBIT}" "${LINK_UTILIZATION_PERCENT}" "${PROTOCOL_OVERHEAD_PERCENT}"
fi
if [[ "${ENABLE_FAIR_QUEUE}" == "1" ]]; then
  if (( SHAPE_MBIT <= 10000 )); then
    printf '  Fair queue:       CAKE at %s Mbps\n' "${SHAPE_MBIT}"
  else
    printf '  Fair queue:       high-throughput fq pacing (CAKE bypassed above 10 Gbps)\n'
  fi
else
  printf '  Fair queue:       disabled by RTMP_ENABLE_FAIR_QUEUE=0\n'
fi
printf '  Admin credential: %s\n\n' "${CREDENTIALS}"
printf 'Next: sign in, create an application, then create a stream and copy its one-time publish key.\n'
