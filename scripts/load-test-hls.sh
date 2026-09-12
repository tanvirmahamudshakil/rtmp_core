#!/usr/bin/env bash
set -Eeuo pipefail

# End-to-end HLS audience test. Run this from one or more external generator
# hosts; running it on the StreamForge VPS measures the generator/loopback,
# not the public delivery path.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

URL="${URL:-}"
VIEWERS="${VIEWERS:-1000}"
RAMP="${RAMP:-5m}"
HOLD="${HOLD:-10m}"
RAMP_DOWN="${RAMP_DOWN:-2m}"
QUALITY="${QUALITY:-auto}"

[[ "${URL}" =~ ^https?:// ]] || {
  printf 'URL must be a full http(s) HLS master/media-playlist URL\n' >&2
  exit 2
}
[[ "${VIEWERS}" =~ ^[1-9][0-9]*$ ]] || {
  printf 'VIEWERS must be a positive integer\n' >&2
  exit 2
}
command -v k6 >/dev/null 2>&1 || {
  printf 'k6 is required on the external load-generator host\n' >&2
  exit 2
}

# One long-lived virtual viewer normally keeps at least one socket. Raise the
# soft limit up to the hard limit when possible, and fail early instead of
# reporting a false server ceiling caused by the generator itself.
required_fds=$((VIEWERS + 4096))
hard_fds="$(ulimit -Hn)"
if [[ "${hard_fds}" == "unlimited" ]]; then
  ulimit -Sn "${required_fds}" 2>/dev/null || true
elif [[ "${hard_fds}" =~ ^[0-9]+$ ]]; then
  target_fds="${required_fds}"
  if (( target_fds > hard_fds )); then target_fds="${hard_fds}"; fi
  ulimit -Sn "${target_fds}" 2>/dev/null || true
fi
soft_fds="$(ulimit -Sn)"
if [[ "${soft_fds}" != "unlimited" ]] && (( soft_fds < required_fds )); then
  printf 'load generator fd limit is %s; need at least %s for %s viewers\n' \
    "${soft_fds}" "${required_fds}" "${VIEWERS}" >&2
  exit 2
fi

printf 'HLS load: viewers=%s ramp=%s hold=%s quality=%s url=%s\n' \
  "${VIEWERS}" "${RAMP}" "${HOLD}" "${QUALITY}" "${URL}"
printf 'Generator must supply the full audience bandwidth; distribute this test when one host cannot.\n'

export URL VIEWERS RAMP HOLD RAMP_DOWN QUALITY
exec k6 run "${PROJECT_DIR}/hls-1500.js"
