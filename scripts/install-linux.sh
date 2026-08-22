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
#   RTMP_ENABLE_FAIR_QUEUE       1 (default) shapes at the configured link
#                                utilization target and fairly schedules viewer
#                                flows, reserving capacity for new joins.
#   RTMP_CONFIGURE_FIREWALL      1 (default) adds rules only if UFW is active.
#   RTMP_CONFIGURE_DNS            1 (default) adds public resolvers (Cloudflare,
#                                Google, Quad9) as systemd-resolved fallback DNS
#                                so source-transcode pulls of an external HLS/RTMP
#                                URL still resolve if the provider's own DNS
#                                fails or blocks a niche domain.
#   RTMP_WORKERS                 Worker count; default min(nproc, 16).
#   RTMP_MAX_CONNECTIONS_PER_IP  Per-source safety limit (default 1000).
#   RTMP_FRESH_INSTALL           1 (default) removes every prior StreamForge
#                                install, database, key, recording and build
#                                artefact before reinstalling. Set 0 only for
#                                the legacy in-place upgrade behaviour.
#   RTMP_FORCE_ROTATE_SECRETS    1 rotates secrets during an in-place install.
#   RTMP_TRANSCODING_RULES       Optional newline-delimited preset rules used
#                                to seed the legacy preset file (the external
#                                supervisor remains disabled; source jobs use
#                                the native in-process pipeline).
#   RTMP_ENABLE_FAST_JOIN        1 (default) sends fresh opens of one configured
#                                master link directly to its startup rendition.
#                                Existing rendition sessions are unaffected.
#   RTMP_FAST_JOIN_APPLICATION   Application for the optimized link (default kk).
#   RTMP_FAST_JOIN_STREAM        Public/base stream name (default KK).
#   RTMP_FAST_JOIN_RENDITION     Startup rendition stream (default KK_480p).
#   RTMP_ENABLE_DISK_GUARD       1 (default) checks disk pressure every 5m.
#   RTMP_DISK_TRIGGER_PERCENT    Start cleanup at this usage (default 80).
#   RTMP_DISK_TARGET_PERCENT     Stop cleanup at this usage (default 70).
#   RTMP_DISK_MIN_FREE_MB        Absolute free-space reserve (default 4096).
#   RTMP_CLEAN_BUILD_ARTIFACTS   1 (default) removes rebuildable build and
#                                node_modules trees after a successful install.
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
CONFIGURE_DNS="${RTMP_CONFIGURE_DNS:-1}"
FORCE_ROTATE="${RTMP_FORCE_ROTATE_SECRETS:-0}"
FRESH_INSTALL="${RTMP_FRESH_INSTALL:-1}"
# The external-process transcoder is intentionally unavailable in production.
# Source-transcode jobs use RTMP_NATIVE_TRANSCODE and never invoke FFmpeg.
ENABLE_TRANSCODING=0
TRANSCODING_RULES="${RTMP_TRANSCODING_RULES:-}"
ENABLE_FAST_JOIN="${RTMP_ENABLE_FAST_JOIN:-1}"
FAST_JOIN_APPLICATION="${RTMP_FAST_JOIN_APPLICATION:-kk}"
FAST_JOIN_STREAM="${RTMP_FAST_JOIN_STREAM:-KK}"
FAST_JOIN_RENDITION="${RTMP_FAST_JOIN_RENDITION:-KK_480p}"
ENABLE_DISK_GUARD="${RTMP_ENABLE_DISK_GUARD:-1}"
DISK_TRIGGER_PERCENT="${RTMP_DISK_TRIGGER_PERCENT:-80}"
DISK_TARGET_PERCENT="${RTMP_DISK_TARGET_PERCENT:-70}"
DISK_MIN_FREE_MB="${RTMP_DISK_MIN_FREE_MB:-4096}"
CLEAN_BUILD_ARTIFACTS="${RTMP_CLEAN_BUILD_ARTIFACTS:-1}"

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
[[ "${CONFIGURE_DNS}" =~ ^[01]$ ]] || die "RTMP_CONFIGURE_DNS must be 0 or 1."
[[ "${FORCE_ROTATE}" =~ ^[01]$ ]] || die "RTMP_FORCE_ROTATE_SECRETS must be 0 or 1."
[[ "${FRESH_INSTALL}" =~ ^[01]$ ]] || die "RTMP_FRESH_INSTALL must be 0 or 1."
if [[ "${RTMP_ENABLE_TRANSCODING:-0}" != "0" ]]; then
  die "RTMP_ENABLE_TRANSCODING is not supported: production source transcoding is FFmpeg-free and native."
fi
[[ "${ENABLE_FAST_JOIN}" =~ ^[01]$ ]] || die "RTMP_ENABLE_FAST_JOIN must be 0 or 1."
[[ "${ENABLE_DISK_GUARD}" =~ ^[01]$ ]] || die "RTMP_ENABLE_DISK_GUARD must be 0 or 1."
[[ "${CLEAN_BUILD_ARTIFACTS}" =~ ^[01]$ ]] || die "RTMP_CLEAN_BUILD_ARTIFACTS must be 0 or 1."
[[ "${DISK_TRIGGER_PERCENT}" =~ ^[0-9]+$ ]] &&
  (( DISK_TRIGGER_PERCENT >= 50 && DISK_TRIGGER_PERCENT <= 98 )) ||
  die "RTMP_DISK_TRIGGER_PERCENT must be between 50 and 98."
[[ "${DISK_TARGET_PERCENT}" =~ ^[0-9]+$ ]] &&
  (( DISK_TARGET_PERCENT >= 40 && DISK_TARGET_PERCENT < DISK_TRIGGER_PERCENT )) ||
  die "RTMP_DISK_TARGET_PERCENT must be between 40 and trigger-1."
[[ "${DISK_MIN_FREE_MB}" =~ ^[0-9]+$ ]] && (( DISK_MIN_FREE_MB >= 512 )) ||
  die "RTMP_DISK_MIN_FREE_MB must be at least 512."
if [[ "${ENABLE_FAST_JOIN}" == "1" ]]; then
  for fast_join_value in "${FAST_JOIN_APPLICATION}" "${FAST_JOIN_STREAM}" "${FAST_JOIN_RENDITION}"; do
    [[ "${fast_join_value}" =~ ^[A-Za-z0-9][A-Za-z0-9_-]*$ ]] ||
      die "Fast-join application/stream/rendition names may contain only letters, numbers, underscores and hyphens."
  done
fi
if [[ -n "${DOMAIN}" && ! "${DOMAIN}" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]]; then
  die "RTMP_DOMAIN must be a hostname without a URL scheme, path, port or whitespace."
fi
if [[ "${EXPECTED_STREAM_MBIT}" != "auto" ]]; then
  awk -v value="${EXPECTED_STREAM_MBIT}" 'BEGIN { exit !(value >= 0.05 && value <= 100) }' ||
    die "RTMP_EXPECTED_STREAM_MBIT must be between 0.05 and 100 Mbps."
fi
awk -v value="${RESOURCE_SIZING_MBIT}" 'BEGIN { exit !(value >= 0.05 && value <= 100) }' ||
  die "RTMP_RESOURCE_SIZING_MBIT must be between 0.05 and 100 Mbps."

remove_managed_path() {
  local target="$1"
  case "${target}" in
    "${SOURCE_DIR}/build" | \
    "${SOURCE_DIR}/admin/node_modules" | \
    "${SOURCE_DIR}/admin/dist" | \
    /etc/rtmp-server | \
    /var/lib/rtmp-server | \
    /var/backups/rtmp-server | \
    /var/www/streamforge | \
    /var/log/rtmp-server | \
    /usr/share/doc/rtmp-server | \
    /etc/systemd/system/rtmp-server.service | \
    /etc/systemd/system/rtmp-server.service.d | \
    /etc/systemd/system/rtmp-network-tune.service | \
    /etc/systemd/system/rtmp-network-tune.service.d | \
    /etc/systemd/system/viewer-estimator.service | \
    /etc/systemd/system/streamforge-disk-guard.service | \
    /etc/systemd/system/streamforge-disk-guard.timer | \
    /etc/systemd/journald.conf.d/streamforge.conf | \
    /etc/sysctl.d/60-streamforge.conf | \
    /etc/default/rtmp-network | \
    /etc/default/streamforge-disk-guard | \
    /etc/logrotate.d/rtmp-server | \
    /usr/local/sbin/rtmp-network-tune | \
    /usr/local/sbin/streamforge-disk-guard | \
    /usr/local/bin/viewer_estimator.py | \
    /root/streamforge-credentials.txt | \
    /etc/caddy/Caddyfile | \
    /etc/systemd/system/caddy.service.d/streamforge.conf | \
    /etc/varnish/streamforge.vcl | \
    /etc/systemd/system/varnish.service.d | \
    /usr/local/bin/rtmp-server*)
      ;;
    *)
      die "Refusing to remove unexpected path '${target}' during fresh-install cleanup."
      ;;
  esac

  if [[ -L "${target}" || -f "${target}" ]]; then
    rm -f -- "${target}"
  elif [[ -d "${target}" ]]; then
    rm -rf -- "${target}"
  fi
}

remove_streamforge_ufw_rules() {
  command -v ufw >/dev/null 2>&1 || return 0
  ufw status 2>/dev/null | grep -q '^Status: active' || return 0

  local rule_number
  while read -r rule_number; do
    [[ "${rule_number}" =~ ^[0-9]+$ ]] || continue
    ufw --force delete "${rule_number}" >/dev/null 2>&1 || true
  done < <(
    ufw status numbered 2>/dev/null |
      awk '/StreamForge (RTMP|HTTP|HTTPS)/ {
        number=$0
        sub(/^[^[]*\[[[:space:]]*/, "", number)
        sub(/\].*$/, "", number)
        gsub(/[^0-9]/, "", number)
        if (number != "") print number
      }' |
      sort -rn
  )
}

stop_and_disable_unit() {
  local unit="$1"
  if systemctl is-active --quiet "${unit}"; then
    if ! systemctl stop "${unit}"; then
      # A service whose graceful stop exceeded TimeoutStopSec can return a
      # failure even after systemd has killed the final process. That state is
      # safe to clean. Refuse only when a live MainPID still exists.
      local main_pid
      main_pid="$(systemctl show --property MainPID --value "${unit}" 2>/dev/null || printf '0')"
      if [[ "${main_pid}" =~ ^[1-9][0-9]*$ ]] && kill -0 "${main_pid}" 2>/dev/null; then
        die "Could not stop ${unit}; refusing to remove files used by PID ${main_pid}."
      fi
      log "${unit} required a forced systemd stop; no process remains, continuing cleanup"
    fi
  fi
  systemctl disable "${unit}" >/dev/null 2>&1 || true
}

clean_previous_install() {
  log "Fresh install requested: removing all previous StreamForge services, data, keys, recordings and builds"
  log "Fresh-install deletion is permanent; new credentials and stream keys will be generated"

  local old_interface=""
  if [[ -r /etc/default/rtmp-network ]]; then
    old_interface="$(sed -n 's/^RTMP_INTERFACE=//p' /etc/default/rtmp-network | tail -n 1)"
  fi

  # Absent units and files are skipped. A unit that exists but cannot stop is
  # a hard failure: deleting its executable or data underneath it is unsafe.
  stop_and_disable_unit rtmp-server.service
  stop_and_disable_unit rtmp-network-tune.service
  stop_and_disable_unit viewer-estimator.service
  stop_and_disable_unit streamforge-disk-guard.timer
  stop_and_disable_unit streamforge-disk-guard.service

  local caddy_config_is_streamforge=0
  if [[ -r /etc/caddy/Caddyfile ]] &&
     grep -Eq 'Managed by StreamForge install-linux\.sh|/var/www/streamforge' /etc/caddy/Caddyfile; then
    caddy_config_is_streamforge=1
    stop_and_disable_unit caddy.service
  fi

  local varnish_config_is_streamforge=0
  if [[ -r /etc/varnish/streamforge.vcl ]] &&
     grep -q 'Managed by StreamForge install-linux.sh' /etc/varnish/streamforge.vcl; then
    varnish_config_is_streamforge=1
    stop_and_disable_unit varnish.service
  fi

  if [[ "${old_interface}" =~ ^[A-Za-z0-9_.:-]+$ ]] &&
     command -v ip >/dev/null 2>&1 &&
     command -v tc >/dev/null 2>&1 &&
     ip link show dev "${old_interface}" >/dev/null 2>&1; then
    tc qdisc del dev "${old_interface}" root >/dev/null 2>&1 || true
  fi

  remove_streamforge_ufw_rules

  local target
  for target in \
    /etc/rtmp-server \
    /var/lib/rtmp-server \
    /var/backups/rtmp-server \
    /var/www/streamforge \
    /var/log/rtmp-server \
    /usr/share/doc/rtmp-server \
    /etc/systemd/system/rtmp-server.service \
    /etc/systemd/system/rtmp-server.service.d \
    /etc/systemd/system/rtmp-network-tune.service \
    /etc/systemd/system/rtmp-network-tune.service.d \
    /etc/systemd/system/viewer-estimator.service \
    /etc/systemd/system/streamforge-disk-guard.service \
    /etc/systemd/system/streamforge-disk-guard.timer \
    /etc/systemd/journald.conf.d/streamforge.conf \
    /etc/sysctl.d/60-streamforge.conf \
    /etc/default/rtmp-network \
    /etc/default/streamforge-disk-guard \
    /etc/logrotate.d/rtmp-server \
    /usr/local/sbin/rtmp-network-tune \
    /usr/local/sbin/streamforge-disk-guard \
    /usr/local/bin/viewer_estimator.py \
    /root/streamforge-credentials.txt \
    /etc/systemd/system/caddy.service.d/streamforge.conf \
    "${SOURCE_DIR}/build" \
    "${SOURCE_DIR}/admin/node_modules" \
    "${SOURCE_DIR}/admin/dist"; do
    remove_managed_path "${target}"
  done

  if [[ "${caddy_config_is_streamforge}" == "1" ]]; then
    remove_managed_path /etc/caddy/Caddyfile
  fi

  if [[ "${varnish_config_is_streamforge}" == "1" ]]; then
    remove_managed_path /etc/varnish/streamforge.vcl
    remove_managed_path /etc/systemd/system/varnish.service.d
  fi

  while IFS= read -r -d '' target; do
    remove_managed_path "${target}"
  done < <(
    find /usr/local/bin -maxdepth 1 \( -type f -o -type l \) -name 'rtmp-server*' -print0 2>/dev/null
  )

  if command -v userdel >/dev/null 2>&1 && id rtmp-server >/dev/null 2>&1; then
    userdel rtmp-server >/dev/null 2>&1 || true
  fi
  if command -v groupdel >/dev/null 2>&1 && getent group rtmp-server >/dev/null 2>&1; then
    groupdel rtmp-server >/dev/null 2>&1 || true
  fi

  systemctl daemon-reload
  systemctl reset-failed rtmp-server.service rtmp-network-tune.service viewer-estimator.service \
    streamforge-disk-guard.service varnish.service >/dev/null 2>&1 || true
  log "Previous StreamForge installation cleanup complete"
}

if [[ "${FRESH_INSTALL}" == "1" ]]; then
  clean_previous_install
fi

BITRATE_MODE="auto"
CAPACITY_STREAM_MBIT="${RESOURCE_SIZING_MBIT}"
EXPECTED_STREAM_JSON="null"
if [[ "${EXPECTED_STREAM_MBIT}" != "auto" ]]; then
  BITRATE_MODE="fixed-capacity-override"
  CAPACITY_STREAM_MBIT="${EXPECTED_STREAM_MBIT}"
  EXPECTED_STREAM_JSON="${EXPECTED_STREAM_MBIT}"
fi

APT_REINSTALL_ARGS=()
if [[ "${FRESH_INSTALL}" == "1" ]]; then
  APT_REINSTALL_ARGS=(--reinstall)
fi

log "Installing or refreshing compiler, runtime, web server and network tooling"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" \
  build-essential cmake ninja-build pkg-config git ca-certificates curl gnupg \
  liburing-dev liburing2 libssl-dev libsqlite3-dev libsqlite3-0 sqlite3 \
  iproute2 ethtool kmod varnish

# The distribution's default "clang" meta-package tracks whatever major
# version that release shipped with — fine on 24.04 (clang-18) but not
# guaranteed on every future point release. Pick the newest clang actually
# available in the configured repositories instead of hardcoding one, and
# verify it is new enough for the C++23 features this codebase uses (see
# cmake/CompilerWarnings.cmake / CMakeLists.txt for the standard flag).
CLANG_MIN_MAJOR=16
CLANG_PACKAGE=""
CLANG_MAJOR=""
apt-cache search --names-only '^clang-[0-9]+$' 2>/dev/null | while read -r name _; do
  echo "${name}"
done > /tmp/streamforge-clang-candidates.$$  || true
if apt-cache show clang >/dev/null 2>&1; then
  CLANG_PACKAGE="clang"
fi
while read -r candidate; do
  major="${candidate#clang-}"
  [[ "${major}" =~ ^[0-9]+$ ]] || continue
  if [[ -z "${CLANG_MAJOR}" ]] || (( major > CLANG_MAJOR )); then
    CLANG_MAJOR="${major}"
    CLANG_PACKAGE="${candidate}"
  fi
done < /tmp/streamforge-clang-candidates.$$
rm -f /tmp/streamforge-clang-candidates.$$
[[ -n "${CLANG_PACKAGE}" ]] || die "No clang package is available from this distribution's configured repositories."
apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" "${CLANG_PACKAGE}"
CLANG_BIN="$(command -v "${CLANG_PACKAGE}" || command -v clang)"
[[ -n "${CLANG_BIN}" ]] || die "clang was installed but is not on PATH."
CLANGXX_BIN="${CLANG_BIN/clang/clang++}"
command -v "${CLANGXX_BIN}" >/dev/null 2>&1 || CLANGXX_BIN="$(command -v clang++)"
DETECTED_CLANG_MAJOR="$("${CLANG_BIN}" --version | sed -n 's/.*version \([0-9]\+\).*/\1/p' | head -n 1)"
[[ "${DETECTED_CLANG_MAJOR}" =~ ^[0-9]+$ ]] || die "Could not determine the installed clang version."
(( DETECTED_CLANG_MAJOR >= CLANG_MIN_MAJOR )) ||
  die "clang ${DETECTED_CLANG_MAJOR} is too old for C++23 (need >= ${CLANG_MIN_MAJOR}); this distribution only offers ${CLANG_PACKAGE}."
log "Using ${CLANG_BIN} (clang ${DETECTED_CLANG_MAJOR})"

# In-process FFmpeg-free transcoding pipeline + source-transcode jobs
# (docs/native-transcoding.md): openh264 (H.264 decode), x265 (HEVC encode),
# x264 (H.264 encode), libfdk-aac (AAC), libyuv (scale), libcurl (HLS source
# pull). It is mandatory in the production installer: there is deliberately no
# FFmpeg fallback.
NATIVE_TRANSCODE=1
if [[ "${RTMP_ENABLE_NATIVE_TRANSCODE:-1}" != "1" ]]; then
  die "RTMP_ENABLE_NATIVE_TRANSCODE=0 is unsupported: no FFmpeg fallback is installed."
fi
apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" \
  libx265-dev libx264-dev libopenh264-dev libfdk-aac-dev libcurl4-openssl-dev libyuv-dev

if ! apt-cache show caddy >/dev/null 2>&1; then
  if [[ "${ID}" == "ubuntu" ]]; then
    # Some Ubuntu point releases carry caddy in universe but haven't enabled
    # it by default; add it first since it's the cheapest fix.
    apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" software-properties-common
    add-apt-repository -y universe
    apt-get update
  fi
fi
if ! apt-cache show caddy >/dev/null 2>&1; then
  # Neither Ubuntu's universe nor Debian's own repositories are guaranteed to
  # carry caddy on every release. Fall back to caddy's own apt repository,
  # which auto-selects the right feed for whatever codename this host
  # reports — no distro/version hardcoded here.
  log "caddy not found in distribution repositories; adding the upstream caddy apt repository"
  apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" debian-keyring debian-archive-keyring apt-transport-https
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' |
    gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
    > /etc/apt/sources.list.d/caddy-stable.list
  apt-get update
fi
apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" caddy ||
  die "Caddy is unavailable from this distribution's configured repositories and the upstream caddy repository."

CMAKE_VERSION="$(cmake --version | awk 'NR == 1 { print $3 }')"
dpkg --compare-versions "${CMAKE_VERSION}" ge 3.25 ||
  die "CMake 3.25+ is required; this distribution provides ${CMAKE_VERSION}."

# The distro-packaged nodejs varies a lot by release (some ship 18.x, some
# 20.x); the admin panel's Vite 6 / React 19 toolchain needs a reasonably
# current runtime. Prefer the distro package when it's new enough, otherwise
# use NodeSource's setup script, which itself auto-detects the codename.
NODE_MIN_MAJOR=18
NODE_OK=0
if apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" nodejs npm 2>/dev/null &&
   command -v node >/dev/null 2>&1; then
  INSTALLED_NODE_MAJOR="$(node --version | sed -n 's/^v\([0-9]\+\).*/\1/p')"
  if [[ "${INSTALLED_NODE_MAJOR}" =~ ^[0-9]+$ ]] && (( INSTALLED_NODE_MAJOR >= NODE_MIN_MAJOR )); then
    NODE_OK=1
  fi
fi
if [[ "${NODE_OK}" != "1" ]]; then
  log "Distribution nodejs is missing or older than ${NODE_MIN_MAJOR}.x; installing from NodeSource"
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt-get install -y --no-install-recommends "${APT_REINSTALL_ARGS[@]}" nodejs
  command -v node >/dev/null 2>&1 || die "Node.js installation failed."
  INSTALLED_NODE_MAJOR="$(node --version | sed -n 's/^v\([0-9]\+\).*/\1/p')"
  [[ "${INSTALLED_NODE_MAJOR}" =~ ^[0-9]+$ ]] && (( INSTALLED_NODE_MAJOR >= NODE_MIN_MAJOR )) ||
    die "NodeSource installation did not provide Node.js >= ${NODE_MIN_MAJOR}."
fi

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
export CC="${CLANG_BIN}"
export CXX="${CLANGXX_BIN}"
# Enable the in-process native transcoding pipeline in the build when its
# libraries were installed above, so source-transcode jobs are available.
NATIVE_TRANSCODE_CMAKE_ARGS=()
if [[ "${NATIVE_TRANSCODE}" == "1" ]]; then
  NATIVE_TRANSCODE_CMAKE_ARGS+=(-DRTMP_ENABLE_NATIVE_TRANSCODE=ON)
fi
cmake --fresh --preset production "${NATIVE_TRANSCODE_CMAKE_ARGS[@]}"
cmake --build --preset production --clean-first --parallel "${CPU_COUNT}"
SERVER_BINARY="${SOURCE_DIR}/build/production/apps/rtmp_server/rtmp_server"
[[ -x "${SERVER_BINARY}" ]] || die "Production server binary was not produced."
SERVER_BINARY_SHA256="$(sha256sum "${SERVER_BINARY}" | awk '{ print $1 }')"
[[ "${SERVER_BINARY_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
  die "Could not calculate the production server binary SHA-256."

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

SERVER_INSTALL_PATH=/usr/local/bin/rtmp-server
SERVER_STAGED_PATH=/usr/local/bin/rtmp-server.new
if [[ -x "${SERVER_INSTALL_PATH}" ]]; then
  cp -a "${SERVER_INSTALL_PATH}" "${SERVER_INSTALL_PATH}.previous"
fi
install -m 0755 -o root -g root "${SERVER_BINARY}" "${SERVER_STAGED_PATH}"
STAGED_BINARY_SHA256="$(sha256sum "${SERVER_STAGED_PATH}" | awk '{ print $1 }')"
if [[ "${STAGED_BINARY_SHA256}" != "${SERVER_BINARY_SHA256}" ]]; then
  rm -f "${SERVER_STAGED_PATH}"
  die "Staged server binary does not match the production build; the current installation was not changed."
fi
# Rename only after verification. A running service keeps its already-open
# executable while new starts see the complete replacement, never a partial
# copy. The .previous binary remains available for an immediate rollback.
mv -f "${SERVER_STAGED_PATH}" "${SERVER_INSTALL_PATH}"
log "Installed verified server binary (SHA-256 ${SERVER_BINARY_SHA256})"
install -m 0640 -o root -g rtmp-server "${SOURCE_DIR}/config/server.example.yaml" /etc/rtmp-server/server.yaml
install -m 0640 -o root -g rtmp-server "${SOURCE_DIR}/config/transcoding.example.conf" \
  /etc/rtmp-server/transcoding.conf
if [[ -n "${TRANSCODING_RULES}" ]]; then
  printf '%s\n' "${TRANSCODING_RULES}" > /etc/rtmp-server/transcoding.conf
  chown root:rtmp-server /etc/rtmp-server/transcoding.conf
  chmod 0640 /etc/rtmp-server/transcoding.conf
fi
install -m 0644 -o root -g root "${SOURCE_DIR}/docs/deployment.md" /usr/share/doc/rtmp-server/deployment.md
install -m 0644 -o root -g root "${SOURCE_DIR}/docs/transcoding.md" /usr/share/doc/rtmp-server/transcoding.md
cp -a "${SOURCE_DIR}/admin/dist/." /var/www/streamforge/
install -d -m 0755 -o root -g root /var/www/streamforge/internal
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
RTMP_SERVER_TRANSCODING_ENABLED=false
RTMP_SERVER_TRANSCODING_PRESET_FILE=/etc/rtmp-server/transcoding.conf
RTMP_SERVER_TRANSCODING_MAX_ACTIVE_JOBS=16
RTMP_SERVER_TRANSCODING_MAX_OUTPUTS_PER_JOB=16
RTMP_SERVER_TRANSCODING_MAX_RESTART_ATTEMPTS=5
RTMP_SERVER_RECORDING_ENABLED=false
RTMP_SERVER_RECORDING_DIRECTORY=/var/lib/rtmp-server/recordings
RTMP_SERVER_LOG_LEVEL=info
RTMP_SERVER_METRICS_ENABLED=true
EOF
chown root:rtmp-server "${EXISTING_ENV}"
chmod 0640 "${EXISTING_ENV}"

# Hardware encoders are optional. Membership exposes /dev/dri and NVIDIA
# character devices when the host has the matching driver; software
# transcoding works without either group.
for encoder_group in video render; do
  if getent group "${encoder_group}" >/dev/null; then
    usermod -a -G "${encoder_group}" rtmp-server
  fi
done

CREDENTIALS=/root/streamforge-credentials.txt
cat > "${CREDENTIALS}" <<EOF
StreamForge Control
===================
Admin URL: $(if [[ -n "${DOMAIN}" ]]; then printf 'https://%s' "${DOMAIN}"; else printf 'http://%s' "${PRIMARY_IP}"; fi)
RTMP origin: rtmp://${PUBLIC_HOST}:1935
Capacity bandwidth: ${BANDWIDTH_MBIT} Mbps (${BANDWIDTH_SOURCE})
Bitrate mode: $(if [[ "${BITRATE_MODE}" == "auto" ]]; then printf 'OBS/transcoder passthrough, measured while live'; else printf 'manual capacity override (%s Mbps)' "${EXPECTED_STREAM_MBIT}"; fi)
Pre-live viewer safety ceiling: ${MAX_VIEWERS}
Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)

The admin panel, RTMP playback, and HLS playback are open and require no access token.
Each stream has one RTMP URL for publisher input/direct RTMP playback:
  rtmp://${PUBLIC_HOST}:1935/<application>/<stream>
For scalable segmented playback in VLC/web players, use:
  $(if [[ -n "${DOMAIN}" ]]; then printf 'https://%s' "${DOMAIN}"; else printf 'http://%s' "${PRIMARY_IP}"; fi)/hls/<application>/<stream>/index.m3u8
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
ConfigurationDirectoryMode=0750
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/rtmp-server
PrivateTmp=true
PrivateDevices=$(if [[ "${ENABLE_TRANSCODING}" == "1" ]]; then printf false; else printf true; fi)
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
MemoryDenyWriteExecute=$(if [[ "${ENABLE_TRANSCODING}" == "1" ]]; then printf false; else printf true; fi)
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

if [[ "${CONFIGURE_DNS}" == "1" ]] && command -v systemctl >/dev/null 2>&1 &&
   systemctl list-unit-files systemd-resolved.service >/dev/null 2>&1; then
  log "Adding public fallback DNS resolvers (source-transcode jobs pull external URLs by hostname)"
  RESOLVED_CONF=/etc/systemd/resolved.conf
  FALLBACK_LINE="FallbackDNS=1.1.1.1 8.8.8.8 9.9.9.9"
  if [[ -r "${RESOLVED_CONF}" ]] && grep -q '^FallbackDNS=' "${RESOLVED_CONF}"; then
    sed -i "s/^FallbackDNS=.*/${FALLBACK_LINE}/" "${RESOLVED_CONF}"
  elif [[ -r "${RESOLVED_CONF}" ]] && grep -q '^\[Resolve\]' "${RESOLVED_CONF}"; then
    sed -i "/^\[Resolve\]/a ${FALLBACK_LINE}" "${RESOLVED_CONF}"
  else
    printf '[Resolve]\n%s\n' "${FALLBACK_LINE}" >> "${RESOLVED_CONF}"
  fi
  systemctl enable --now systemd-resolved.service >/dev/null 2>&1 || true
  resolvectl flush-caches >/dev/null 2>&1 || true
  systemctl restart systemd-resolved.service
else
  [[ "${CONFIGURE_DNS}" == "1" ]] &&
    log "systemd-resolved not present; skipping fallback DNS configuration"
fi

log "Configuring NIC ${PRIMARY_INTERFACE}"
ethtool -K "${PRIMARY_INTERFACE}" gro on gso on tso on >/dev/null 2>&1 || true
if [[ "${ENABLE_FAIR_QUEUE}" == "1" ]]; then
  # Keeping the queue on this VPS instead of the provider's opaque policer is
  # what lets a new viewer's playlist/first segment compete fairly with the
  # already-active segment flows. The unused percentage is join/burst reserve.
  SHAPE_MBIT=$((BANDWIDTH_MBIT * LINK_UTILIZATION_PERCENT / 100))
  cat > /etc/default/rtmp-network <<EOF
RTMP_INTERFACE=${PRIMARY_INTERFACE}
RTMP_SHAPE_MBIT=${SHAPE_MBIT}
RTMP_QUEUE_COUNT=${WORKERS}
EOF
  cat > /usr/local/sbin/rtmp-network-tune <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. /etc/default/rtmp-network
[[ -n "${RTMP_INTERFACE:-}" && "${RTMP_SHAPE_MBIT:-0}" =~ ^[0-9]+$ ]] || exit 1

# NIC queue count matched to WorkerPool worker count, so RSS spreads
# interrupts/softirqs across the same cores the io_uring workers run on
# instead of funnelling every packet through one queue/core. Best-effort:
# most virtualised NICs (virtio-net on KVM-based VPS providers) expose a
# fixed single queue and reject -L, so failure here is expected and silent.
if [[ "${RTMP_QUEUE_COUNT:-0}" =~ ^[0-9]+$ ]] && (( RTMP_QUEUE_COUNT > 1 )); then
  MAX_QUEUES="$(ethtool -l "${RTMP_INTERFACE}" 2>/dev/null |
    awk '/^Combined:/{q=$2} END{print q+0}')"
  if (( MAX_QUEUES > 1 )); then
    TARGET_QUEUES=$(( RTMP_QUEUE_COUNT < MAX_QUEUES ? RTMP_QUEUE_COUNT : MAX_QUEUES ))
    ethtool -L "${RTMP_INTERFACE}" combined "${TARGET_QUEUES}" >/dev/null 2>&1 || true
  fi
fi
# Larger rx/tx rings absorb short bursts (viewer stampede, keyframe spikes)
# without drops. Not all drivers/providers honour this value; ignored if not.
ethtool -G "${RTMP_INTERFACE}" rx 4096 tx 4096 >/dev/null 2>&1 || true

if (( RTMP_SHAPE_MBIT <= 10000 )) && modprobe sch_cake 2>/dev/null; then
  tc qdisc replace dev "${RTMP_INTERFACE}" root cake bandwidth "${RTMP_SHAPE_MBIT}Mbit" \
    besteffort dual-dsthost nat nowash
else
  # CAKE becomes CPU-expensive at 10G+ line rates. HTB provides the required
  # join headroom while fq fairly schedules the individual HTTP flows.
  tc qdisc replace dev "${RTMP_INTERFACE}" root handle 1: htb default 10
  tc class replace dev "${RTMP_INTERFACE}" parent 1: classid 1:10 htb \
    rate "${RTMP_SHAPE_MBIT}Mbit" ceil "${RTMP_SHAPE_MBIT}Mbit" burst 16m cburst 16m
  tc qdisc replace dev "${RTMP_INTERFACE}" parent 1:10 handle 10: \
    fq limit 100000 flow_limit 1000 buckets 65536
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

log "Bounding system logs and configuring cache-edge delivery accounting"
# Keep the distro access-log writer disabled; the bounded estimator consumes
# Varnish's shared-memory log directly and writes only a tiny aggregate JSON
# snapshot, never one disk record per HLS request.
systemctl disable --now varnishncsa.service >/dev/null 2>&1 || true
install -m 0755 -o root -g root "${SOURCE_DIR}/deploy/viewer-estimator/viewer_estimator.py" \
  /usr/local/bin/viewer_estimator.py
install -m 0644 -o root -g root "${SOURCE_DIR}/deploy/viewer-estimator/viewer-estimator.service" \
  /etc/systemd/system/viewer-estimator.service
install -d -m 0755 /etc/systemd/journald.conf.d
install -m 0644 "${SOURCE_DIR}/deploy/systemd/streamforge-journald.conf" \
  /etc/systemd/journald.conf.d/streamforge.conf
systemctl restart systemd-journald.service

if [[ "${ENABLE_DISK_GUARD}" == "1" ]]; then
  log "Installing pressure-triggered disk guard"
  install -m 0755 -o root -g root \
    "${SOURCE_DIR}/deploy/disk-guard/streamforge-disk-guard.sh" \
    /usr/local/sbin/streamforge-disk-guard
  install -m 0644 -o root -g root \
    "${SOURCE_DIR}/deploy/disk-guard/streamforge-disk-guard.service" \
    /etc/systemd/system/streamforge-disk-guard.service
  install -m 0644 -o root -g root \
    "${SOURCE_DIR}/deploy/disk-guard/streamforge-disk-guard.timer" \
    /etc/systemd/system/streamforge-disk-guard.timer
  cat > /etc/default/streamforge-disk-guard <<EOF
STREAMFORGE_DISK_TRIGGER_PERCENT=${DISK_TRIGGER_PERCENT}
STREAMFORGE_DISK_TARGET_PERCENT=${DISK_TARGET_PERCENT}
STREAMFORGE_DISK_MIN_FREE_MB=${DISK_MIN_FREE_MB}
STREAMFORGE_DISK_MIN_RECORDINGS=3
STREAMFORGE_DISK_MIN_BACKUPS=3
STREAMFORGE_STALE_PART_MINUTES=1440
EOF
  chown root:root /etc/default/streamforge-disk-guard
  chmod 0644 /etc/default/streamforge-disk-guard
else
  systemctl disable --now streamforge-disk-guard.timer >/dev/null 2>&1 || true
fi

log "Configuring Varnish shared HLS cache"
# Varnish sits between Caddy and the origin (127.0.0.1:8080) for /hls/*
# traffic only. Public playlists are micro-cached and immutable .ts segments
# are cached in RAM, so viewer count does not multiply origin work. Query
# strings are normalized by the public high-scale VCL to prevent cache-key
# fragmentation.
# Bound to loopback only: never exposed directly to the internet.
MEM_TOTAL_KB="$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)"
[[ "${MEM_TOTAL_KB}" =~ ^[0-9]+$ ]] || die "Could not determine total system memory."
VARNISH_CACHE_MB=$(( MEM_TOTAL_KB / 1024 / 5 ))
if (( VARNISH_CACHE_MB < 128 )); then VARNISH_CACHE_MB=128; fi
if (( VARNISH_CACHE_MB > 16384 )); then VARNISH_CACHE_MB=16384; fi
VARNISH_THREAD_POOLS=$(( (WORKERS + 3) / 4 ))
if (( VARNISH_THREAD_POOLS < 2 )); then VARNISH_THREAD_POOLS=2; fi
if (( VARNISH_THREAD_POOLS > 8 )); then VARNISH_THREAD_POOLS=8; fi

install -m 0644 "${SOURCE_DIR}/deploy/varnish/streamforge.vcl" /etc/varnish/streamforge.vcl

install -d -m 0755 /etc/systemd/system/varnish.service.d
cat > /etc/systemd/system/varnish.service.d/streamforge.conf <<EOF
[Service]
ExecStart=
ExecStart=/usr/sbin/varnishd -j unix,user=vcache -F -a 127.0.0.1:6081 -T localhost:6082 -f /etc/varnish/streamforge.vcl -S /etc/varnish/secret -s malloc,${VARNISH_CACHE_MB}m -p thread_pools=${VARNISH_THREAD_POOLS} -p thread_pool_min=100 -p thread_pool_max=2500 -p thread_queue_limit=100000 -p listen_depth=65535
LimitNOFILE=${NOFILE}
EOF

install -d -m 0755 /etc/systemd/system/caddy.service.d
cat > /etc/systemd/system/caddy.service.d/streamforge.conf <<EOF
[Service]
LimitNOFILE=${NOFILE}
EOF

log "Configuring Caddy admin endpoint"
if [[ -n "${DOMAIN}" ]]; then
  CADDY_SITE="${DOMAIN}"
else
  CADDY_SITE=":80"
fi
FAST_JOIN_CADDY_BLOCK=""
if [[ "${ENABLE_FAST_JOIN}" == "1" ]]; then
  FAST_JOIN_CADDY_BLOCK="$(cat <<EOF
    @fast_join path /hls/${FAST_JOIN_APPLICATION}/${FAST_JOIN_STREAM}/master.m3u8
    handle @fast_join {
        # Start a fresh player on the smaller shared-cache rendition.
        uri replace /hls/${FAST_JOIN_APPLICATION}/${FAST_JOIN_STREAM}/master.m3u8 /hls/${FAST_JOIN_APPLICATION}/${FAST_JOIN_RENDITION}/index.m3u8
        reverse_proxy 127.0.0.1:6081
    }

EOF
)"
fi
cat > /etc/caddy/Caddyfile <<EOF
# Managed by StreamForge install-linux.sh
${CADDY_SITE} {
    # Avoid per-request compression CPU on the high-volume media path.
    @compressible not path /hls/*
    encode @compressible zstd gzip

    @control path /api/*
    handle @control {
        uri strip_prefix /api
        reverse_proxy 127.0.0.1:8080
    }

    @edge_stats path /internal/viewer_estimate.json
    handle @edge_stats {
        root * /var/www/streamforge
        header Cache-Control "no-store"
        file_server
    }

${FAST_JOIN_CADDY_BLOCK}
    # Both playlists and segments use the local shared cache.
    @hls path /hls/*
    handle @hls {
        reverse_proxy 127.0.0.1:6081
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
chown root:root /etc/caddy/Caddyfile
chmod 0644 /etc/caddy/Caddyfile
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
systemctl enable rtmp-server.service >/dev/null
systemctl restart rtmp-server.service

log "Waiting for the origin readiness check before starting the cache layer"
ORIGIN_READY=0
for _ in $(seq 1 30); do
  if curl -fsS http://127.0.0.1:8080/health/ready >/dev/null; then
    ORIGIN_READY=1
    break
  fi
  sleep 1
done
if [[ "${ORIGIN_READY}" != "1" ]]; then
  journalctl -u rtmp-server.service -n 80 --no-pager >&2 || true
  die "The origin did not become ready. Review the journal output above."
fi

systemctl enable --now varnish.service >/dev/null
systemctl restart varnish.service
VARNISH_READY=0
for _ in $(seq 1 15); do
  if curl -fsS -o /dev/null http://127.0.0.1:6081/hls/ 2>/dev/null || \
     systemctl is-active --quiet varnish.service; then
    VARNISH_READY=1
    break
  fi
  sleep 1
done
[[ "${VARNISH_READY}" == "1" ]] || die "Varnish did not start; check 'journalctl -u varnish.service'."

systemctl enable --now viewer-estimator.service >/dev/null
systemctl restart viewer-estimator.service

systemctl enable --now caddy.service >/dev/null
systemctl restart caddy.service

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
systemctl is-active --quiet caddy.service ||
  { journalctl -u caddy.service -n 40 --no-pager >&2 || true; die "caddy.service is not active."; }
EDGE_STATS_READY=0
for _ in $(seq 1 10); do
  if systemctl is-active --quiet viewer-estimator.service &&
     [[ -s /var/www/streamforge/internal/viewer_estimate.json ]]; then
    EDGE_STATS_READY=1
    break
  fi
  sleep 1
done
if [[ "${EDGE_STATS_READY}" != "1" ]]; then
  journalctl -u viewer-estimator.service -n 40 --no-pager >&2 || true
  die "Cache-edge viewer/bandwidth accounting did not become ready."
fi

if [[ "${CLEAN_BUILD_ARTIFACTS}" == "1" ]]; then
  log "Removing rebuildable production build artifacts"
  remove_managed_path "${SOURCE_DIR}/build"
  remove_managed_path "${SOURCE_DIR}/admin/node_modules"
fi
if [[ "${ENABLE_DISK_GUARD}" == "1" ]]; then
  systemctl enable --now streamforge-disk-guard.timer >/dev/null
  # Run after rebuildable artifacts are gone so pressure cleanup never removes
  # a backup or recording merely to preserve a disposable compiler tree.
  systemctl start streamforge-disk-guard.service >/dev/null 2>&1 ||
    log "Disk guard could not reach its reserve; inspect journalctl -u streamforge-disk-guard.service"
fi

ADMIN_URL="http://${PRIMARY_IP}"
if [[ -n "${DOMAIN}" ]]; then ADMIN_URL="https://${DOMAIN}"; fi

printf '\n\033[1;32mStreamForge installation complete.\033[0m\n'
printf '  Admin panel:      %s\n' "${ADMIN_URL}"
printf '  RTMP origin:      rtmp://%s:1935\n' "${PUBLIC_HOST}"
printf '  RTMP stream:      rtmp://%s:1935/<application>/<stream>\n' "${PUBLIC_HOST}"
printf '  HLS playback:     %s/hls/<application>/<stream>/index.m3u8 (no token)\n' "${ADMIN_URL}"
printf '  Install mode:     %s\n' "$(if [[ "${FRESH_INSTALL}" == "1" ]]; then printf 'full clean install'; else printf 'in-place install'; fi)"
printf '  Network device:   %s\n' "${PRIMARY_INTERFACE}"
printf '  Link bandwidth:   %s Mbps — %s\n' "${BANDWIDTH_MBIT}" "${BANDWIDTH_SOURCE}"
printf '  Media workers:    %s\n' "${WORKERS}"
printf '  HLS shared cache:  Varnish, %s MB RAM, 127.0.0.1:6081 (.ts 1h; media playlists 1s; masters 30s)\n' "${VARNISH_CACHE_MB}"
if [[ "${ENABLE_DISK_GUARD}" == "1" ]]; then
  printf '  Disk guard:        every 5m; cleanup at %s%% or <%s MB free, target %s%%\n' \
    "${DISK_TRIGGER_PERCENT}" "${DISK_MIN_FREE_MB}" "${DISK_TARGET_PERCENT}"
else
  printf '  Disk guard:        disabled by RTMP_ENABLE_DISK_GUARD=0\n'
fi
if [[ "${ENABLE_FAST_JOIN}" == "1" ]]; then
  printf '  Fast join:         /hls/%s/%s/master.m3u8 -> %s\n' \
    "${FAST_JOIN_APPLICATION}" "${FAST_JOIN_STREAM}" "${FAST_JOIN_RENDITION}"
else
  printf '  Fast join:         disabled by RTMP_ENABLE_FAST_JOIN=0\n'
fi
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
    printf '  Join reserve:     CAKE fair queue at %s Mbps (%s%% of link)\n' "${SHAPE_MBIT}" "${LINK_UTILIZATION_PERCENT}"
  else
    printf '  Join reserve:     HTB + fq at %s Mbps (%s%% of link)\n' "${SHAPE_MBIT}" "${LINK_UTILIZATION_PERCENT}"
  fi
else
  printf '  Join reserve:     disabled by RTMP_ENABLE_FAIR_QUEUE=0\n'
fi
if [[ "${CONFIGURE_DNS}" == "1" ]]; then
  printf '  Fallback DNS:     1.1.1.1, 8.8.8.8, 9.9.9.9 (via systemd-resolved)\n'
else
  printf '  Fallback DNS:     disabled by RTMP_CONFIGURE_DNS=0\n'
fi
printf '  Install details:  %s\n\n' "${CREDENTIALS}"
printf 'Next: open the panel directly, create a stream, then copy its one RTMP URL for both input and output.\n'
