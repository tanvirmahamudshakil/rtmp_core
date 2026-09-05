#!/usr/bin/env bash
# Example provision-edge hook: "warm pool" pattern, no cloud API required.
# autoscaler.sh calls this (if installed executable as .../provision-edge)
# with the target region as $1 when utilization has stayed above the
# high-water mark.
#
# Assumes a small pool of already-provisioned, already-configured (Caddy +
# Varnish installed, install-edge.sh already run once) but currently idle
# machines, reachable by SSH, whose heartbeat timer is disabled until needed.
# Scaling out costs one SSH round trip and a few seconds — no VM boot time, no
# cloud credentials beyond an SSH key. See docs/high-availability.md "Warm
# pool alternative" for when this fits and when it does not (a fixed pool has
# a ceiling; a true elastic cloud-API hook is the other example this directory
# would otherwise hold, and is entirely provider-specific).
#
# WARM_POOL_HOSTS: space-separated idle hosts for this call's region, e.g.
#   WARM_POOL_HOSTS_eu="edge-eu-standby-1 edge-eu-standby-2"
# A hook failure here is logged by autoscaler.sh and never blocks the next
# poll — the next sustained-high-utilization streak simply tries again.
set -Eeuo pipefail

region="${1:-}"
pool_var="WARM_POOL_HOSTS_${region:-default}"
pool="${!pool_var:-${WARM_POOL_HOSTS_default:-}}"
[[ -n "${pool}" ]] || { echo "no warm-pool hosts configured for region '${region}'" >&2; exit 1; }

for host in ${pool}; do
    if ssh -o ConnectTimeout=5 -o BatchMode=yes "root@${host}" \
        'systemctl is-enabled --quiet streamforge-node-heartbeat.timer 2>/dev/null'; then
        continue # already active, try the next one
    fi
    echo "activating warm-pool host ${host} for region '${region}'"
    ssh -o ConnectTimeout=5 -o BatchMode=yes "root@${host}" \
        'systemctl enable --now streamforge-node-heartbeat.timer'
    exit 0
done

echo "no idle warm-pool host available for region '${region}' -- pool exhausted" >&2
exit 1
