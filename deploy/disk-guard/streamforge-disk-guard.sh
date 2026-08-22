#!/usr/bin/env bash
# Keep the StreamForge root filesystem out of the ENOSPC failure zone.
#
# Cleanup is pressure-triggered, not periodic deletion for its own sake. The
# active SQLite database, configuration, credentials and current `.part`
# recordings are never candidates. Only bounded system caches, stale crash
# artifacts, old backup snapshots and completed recordings are considered.

set -Eeuo pipefail
IFS=$'\n\t'
PATH=/usr/sbin:/usr/bin:/sbin:/bin

CHECK_PATH="${STREAMFORGE_DISK_CHECK_PATH:-/var/lib/rtmp-server}"
RECORDING_DIR="${STREAMFORGE_RECORDING_DIR:-/var/lib/rtmp-server/recordings}"
BACKUP_DIR="${STREAMFORGE_BACKUP_DIR:-/var/backups/rtmp-server}"
TRIGGER_PERCENT="${STREAMFORGE_DISK_TRIGGER_PERCENT:-80}"
TARGET_PERCENT="${STREAMFORGE_DISK_TARGET_PERCENT:-70}"
MIN_FREE_MB="${STREAMFORGE_DISK_MIN_FREE_MB:-4096}"
MIN_RECORDINGS="${STREAMFORGE_DISK_MIN_RECORDINGS:-3}"
MIN_BACKUPS="${STREAMFORGE_DISK_MIN_BACKUPS:-3}"
STALE_PART_MINUTES="${STREAMFORGE_STALE_PART_MINUTES:-1440}"

log() { printf 'streamforge-disk-guard: %s\n' "$*"; }

is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }
for value in "${TRIGGER_PERCENT}" "${TARGET_PERCENT}" "${MIN_FREE_MB}" \
             "${MIN_RECORDINGS}" "${MIN_BACKUPS}" "${STALE_PART_MINUTES}"; do
  is_uint "${value}" || { log "invalid numeric configuration"; exit 2; }
done
(( TRIGGER_PERCENT >= 50 && TRIGGER_PERCENT <= 98 )) || { log "trigger percent must be 50-98"; exit 2; }
(( TARGET_PERCENT >= 40 && TARGET_PERCENT < TRIGGER_PERCENT )) || {
  log "target percent must be 40..trigger-1"
  exit 2
}
(( MIN_FREE_MB >= 512 )) || { log "minimum free space must be at least 512 MB"; exit 2; }
[[ -d "${CHECK_PATH}" ]] || { log "check path does not exist: ${CHECK_PATH}"; exit 1; }

disk_state() {
  df -Pk -- "${CHECK_PATH}" | awk 'NR == 2 {
    used=$5; sub(/%$/, "", used);
    printf "%s\t%s\n", used, int($4 / 1024)
  }'
}

IFS=$'\t' read -r used_percent free_mb < <(disk_state)
is_uint "${used_percent}" && is_uint "${free_mb}" || { log "could not read filesystem usage"; exit 1; }

if [[ "${1:-}" == "--status" ]]; then
  log "used=${used_percent}% free=${free_mb}MB trigger=${TRIGGER_PERCENT}% target=${TARGET_PERCENT}% reserve=${MIN_FREE_MB}MB"
  exit 0
elif (( $# > 0 )); then
  log "usage: streamforge-disk-guard [--status]"
  exit 2
fi

under_pressure() {
  (( used_percent >= TRIGGER_PERCENT || free_mb < MIN_FREE_MB ))
}

target_reached() {
  (( used_percent <= TARGET_PERCENT && free_mb >= MIN_FREE_MB ))
}

refresh_state() {
  IFS=$'\t' read -r used_percent free_mb < <(disk_state)
}

if ! under_pressure; then
  exit 0
fi

log "cleanup started: used=${used_percent}% free=${free_mb}MB trigger=${TRIGGER_PERCENT}% reserve=${MIN_FREE_MB}MB"

# These are bounded/rebuildable system caches. Failures are non-fatal so one
# unavailable subsystem cannot prevent cleanup of the remaining safe targets.
journalctl --rotate >/dev/null 2>&1 || true
journalctl --vacuum-size=64M --vacuum-time=3d >/dev/null 2>&1 || true
apt-get clean >/dev/null 2>&1 || true
systemd-tmpfiles --clean >/dev/null 2>&1 || true

# A `.part` modified in the last day may still belong to a live/slow recording.
# For older files, prove that the current server process has no open descriptor
# before unlinking. If its PID cannot be established, preserve every part.
remove_stale_parts() {
  [[ -d "${RECORDING_DIR}" ]] || return 0
  local server_pid
  server_pid="$(systemctl show --property MainPID --value rtmp-server.service 2>/dev/null || printf 0)"
  [[ "${server_pid}" =~ ^[0-9]+$ ]] || server_pid=0
  if (( server_pid == 0 )) && systemctl is-active --quiet rtmp-server.service 2>/dev/null; then
    log "rtmp-server is active but its PID is unavailable; preserving stale parts"
    return 0
  fi
  if (( server_pid > 0 )) && [[ ! -d "/proc/${server_pid}/fd" ]]; then
    log "could not inspect rtmp-server file descriptors; preserving stale parts"
    return 0
  fi

  local path fd target is_open
  while IFS= read -r path; do
    [[ -n "${path}" ]] || continue
    is_open=0
    if (( server_pid > 0 )); then
      for fd in /proc/"${server_pid}"/fd/*; do
        target="$(readlink -- "${fd}" 2>/dev/null || true)"
        if [[ "${target}" == "${path}" || "${target}" == "${path} (deleted)" ]]; then
          is_open=1
          break
        fi
      done
    fi
    if (( is_open == 0 )) && unlink -- "${path}" 2>/dev/null; then
      log "removed stale recording part ${path}"
    fi
  done < <(find "${RECORDING_DIR}" -xdev -type f -name '*.part' \
             -mmin "+${STALE_PART_MINUTES}" -print 2>/dev/null)
}

remove_stale_parts

refresh_state
target_reached && { log "cleanup complete after cache vacuum: used=${used_percent}% free=${free_mb}MB"; exit 0; }

delete_oldest_until_safe() {
  local directory="$1"
  local pattern="$2"
  local minimum_to_keep="$3"
  [[ -d "${directory}" ]] || return 0

  local -a files=()
  while IFS= read -r path; do
    [[ -n "${path}" ]] && files+=("${path}")
  done < <(find "${directory}" -xdev -type f -name "${pattern}" \
             -printf '%T@ %p\n' 2>/dev/null | sort -n | sed 's/^[^ ]* //')

  local remaining=${#files[@]}
  local path size
  for path in "${files[@]}"; do
    target_reached && break
    (( remaining > minimum_to_keep )) || break
    size="$(stat -c '%s' -- "${path}" 2>/dev/null || printf 0)"
    if [[ "${size}" =~ ^[0-9]+$ ]] && unlink -- "${path}" 2>/dev/null; then
      log "removed ${path} ($((size / 1024 / 1024))MB)"
      remaining=$((remaining - 1))
      refresh_state
    fi
  done
}

# Old database snapshots go before user media, but retain multiple rollback
# points. The live database under /var/lib/rtmp-server is never scanned.
delete_oldest_until_safe "${BACKUP_DIR}" 'rtmp-*.db.gz' "${MIN_BACKUPS}"
delete_oldest_until_safe "${BACKUP_DIR}" 'rtmp-*.db' "${MIN_BACKUPS}"
delete_oldest_until_safe "${RECORDING_DIR}" '*.flv' "${MIN_RECORDINGS}"

refresh_state
if target_reached; then
  log "cleanup complete: used=${used_percent}% free=${free_mb}MB"
else
  log "WARNING safe cleanup exhausted: used=${used_percent}% free=${free_mb}MB"
  exit 1
fi
