#!/usr/bin/env bash
# StreamForge edge autoscaler: polls the origin's capacity signal and calls
# provider-specific hooks to add or remove edge capacity. This script owns
# the decision (when, hysteresis, drain-before-terminate); it never talks to
# a cloud API itself — there is no single API to talk to across every
# provider a deployment might use. See docs/high-availability.md "3.
# Auto-provisioning" for the full design and a no-cloud-API "warm pool"
# alternative.
#
# Run this anywhere that can reach the origin's management API — the origin
# itself, or a small always-on control host. It is not part of rtmp_server
# and holds no state beyond a few in-memory counters, so restarting it only
# resets the hysteresis window, never the fleet.
#
# Environment:
#   STREAMFORGE_MANAGEMENT_URL       (required) origin base URL serving /v1.
#   STREAMFORGE_AUTOSCALE_REGION     region to request capacity/provisioning
#                                    for. Empty = the deployment's only region.
#   STREAMFORGE_AUTOSCALE_HOOKS_DIR  directory holding executable
#                                    `provision-edge` and `deprovision-edge`
#                                    scripts. Default
#                                    /etc/streamforge/autoscale-hooks.
#   STREAMFORGE_AUTOSCALE_POLL_INTERVAL      seconds between polls. Default 30.
#   STREAMFORGE_AUTOSCALE_HIGH_WATER         scale-out threshold, 0-1.
#                                            Default 0.85 (matches the
#                                            origin's own default high_water).
#   STREAMFORGE_AUTOSCALE_LOW_WATER          scale-in threshold, 0-1. Default
#                                            0.40.
#   STREAMFORGE_AUTOSCALE_SCALE_OUT_STREAK   consecutive high polls before
#                                            provisioning. Default 3 (with the
#                                            default 30s interval, ~90s of
#                                            sustained high utilisation before
#                                            acting -- a short traffic spike
#                                            must not trigger a launch that
#                                            outlives it).
#   STREAMFORGE_AUTOSCALE_SCALE_IN_STREAK    consecutive low polls before
#                                            draining a node. Default 10 (~5
#                                            minutes) -- scaling in is far
#                                            more conservative than scaling
#                                            out: over-provisioning costs
#                                            money, under-provisioning costs
#                                            viewers.
#   STREAMFORGE_AUTOSCALE_MIN_EDGES           never drain below this many
#                                            healthy edges. Default 1.
#   STREAMFORGE_AUTOSCALE_DRAIN_TIMEOUT       seconds to wait for a draining
#                                            node's viewers to reach zero
#                                            before deprovisioning it anyway.
#                                            Default 900 (15 min).
set -Eeuo pipefail
IFS=$'\n\t'
PATH=/usr/sbin:/usr/bin:/sbin:/bin

MGMT="${STREAMFORGE_MANAGEMENT_URL:?STREAMFORGE_MANAGEMENT_URL is required}"
REGION="${STREAMFORGE_AUTOSCALE_REGION:-}"
HOOKS_DIR="${STREAMFORGE_AUTOSCALE_HOOKS_DIR:-/etc/streamforge/autoscale-hooks}"
POLL_INTERVAL="${STREAMFORGE_AUTOSCALE_POLL_INTERVAL:-30}"
HIGH_WATER="${STREAMFORGE_AUTOSCALE_HIGH_WATER:-0.85}"
LOW_WATER="${STREAMFORGE_AUTOSCALE_LOW_WATER:-0.40}"
SCALE_OUT_STREAK="${STREAMFORGE_AUTOSCALE_SCALE_OUT_STREAK:-3}"
SCALE_IN_STREAK="${STREAMFORGE_AUTOSCALE_SCALE_IN_STREAK:-10}"
MIN_EDGES="${STREAMFORGE_AUTOSCALE_MIN_EDGES:-1}"
DRAIN_TIMEOUT="${STREAMFORGE_AUTOSCALE_DRAIN_TIMEOUT:-900}"

log() { printf '%s streamforge-autoscaler: %s\n' "$(date -u +%FT%TZ)" "$*"; }

run_hook() {
    local hook="${HOOKS_DIR}/$1"
    shift
    if [[ ! -x "${hook}" ]]; then
        log "hook $1 not installed at ${hook} -- nothing to do"
        return 1
    fi
    "${hook}" "$@"
}

# Extracts one top-level JSON field by name -- a quoted string ("role":"edge")
# or a bare numeric/boolean token ("active_viewers":900, "healthy":true). The
# responses this script reads are small, flat, server-generated objects
# (never user text), so this is safe without a JSON library -- same approach
# deploy/edge/streamforge-node-heartbeat.sh already uses. Quoted-string
# extraction must come first: role/id/region values would otherwise match the
# bare-token branch's character class on their surrounding characters and
# silently return nothing (a real bug this shipped with once already).
json_field() {
    local json="$1" field="$2" match
    match="$(printf '%s' "${json}" | grep -o "\"${field}\" *: *\"[^\"]*\"" | head -1)"
    if [[ -n "${match}" ]]; then
        printf '%s' "${match}" | sed 's/.*:"\(.*\)"$/\1/'
        return
    fi
    printf '%s' "${json}" | grep -o "\"${field}\" *: *[0-9.a-z]*" | head -1 | grep -o '[0-9.a-z]*$'
}

fetch_capacity() {
    curl -sS --max-time 10 "${MGMT%/}/v1/cluster/capacity"
}

fetch_nodes() {
    curl -sS --max-time 10 "${MGMT%/}/v1/cluster/nodes"
}

drain_node() {
    local id="$1"
    curl -sS --max-time 10 -X PATCH "${MGMT%/}/v1/cluster/nodes/${id}" \
        -H 'Content-Type: application/json' --data '{"draining":true}' >/dev/null
}

undrain_node() {
    local id="$1"
    curl -sS --max-time 10 -X PATCH "${MGMT%/}/v1/cluster/nodes/${id}" \
        -H 'Content-Type: application/json' --data '{"draining":false}' >/dev/null
}

remove_node() {
    local id="$1"
    curl -sS --max-time 10 -X DELETE "${MGMT%/}/v1/cluster/nodes/${id}" >/dev/null
}

# Picks the least-loaded healthy, non-draining edge NOT already flagged
# draining by us this run -- the candidate to remove first if scaling in.
# Reads the node list once and does the arithmetic in one awk pass; each
# input line is one node's tab-separated fields extracted by grep/sed above.
least_loaded_edge_id() {
    local nodes_json="$1"
    # One line per node object: {...}. This repo's own JSON never nests an
    # object inside a top-level array element beyond one level for this
    # endpoint, so splitting on "},{" is safe here specifically.
    printf '%s' "${nodes_json}" | grep -o '{"id":"[^}]*}' | while read -r node; do
        local role active capacity healthy draining id
        role="$(json_field "${node}" role)"
        [[ "${role}" == "edge" ]] || continue
        healthy="$(json_field "${node}" healthy)"
        [[ "${healthy}" == "true" ]] || continue
        draining="$(json_field "${node}" draining)"
        [[ "${draining}" == "true" ]] && continue
        id="$(printf '%s' "${node}" | grep -o '"id":"[^"]*"' | head -1 | cut -d'"' -f4)"
        active="$(json_field "${node}" active_viewers)"
        capacity="$(json_field "${node}" capacity_viewers)"
        [[ "${capacity:-0}" -eq 0 ]] && capacity=1 # avoid divide-by-zero; unsized nodes sort last anyway
        printf '%s %s\n' "$(( (active * 1000) / capacity ))" "${id}"
    done | sort -n | head -1 | awk '{print $2}'
}

active_viewers_of() {
    local nodes_json="$1" target_id="$2"
    printf '%s' "${nodes_json}" | grep -o '{"id":"[^}]*}' | while read -r node; do
        local id
        id="$(printf '%s' "${node}" | grep -o '"id":"[^"]*"' | head -1 | cut -d'"' -f4)"
        [[ "${id}" == "${target_id}" ]] || continue
        json_field "${node}" active_viewers
    done
}

log "starting (origin=${MGMT} region='${REGION:-<any>}' high=${HIGH_WATER} low=${LOW_WATER})"

high_streak=0
low_streak=0
draining_id=""
drain_started_at=0

while true; do
    capacity_json="$(fetch_capacity)" || { log "capacity poll failed"; sleep "${POLL_INTERVAL}"; continue; }
    utilization="$(json_field "${capacity_json}" utilization)"
    healthy_edges="$(json_field "${capacity_json}" healthy_edges)"
    utilization="${utilization:-0}"
    healthy_edges="${healthy_edges:-0}"

    log "poll: utilization=${utilization} healthy_edges=${healthy_edges}"

    # --- scale out -----------------------------------------------------
    if awk -v u="${utilization}" -v h="${HIGH_WATER}" 'BEGIN{exit !(u>=h)}'; then
        high_streak=$((high_streak + 1))
        low_streak=0
    else
        high_streak=0
    fi

    if (( high_streak >= SCALE_OUT_STREAK )); then
        log "utilization >= ${HIGH_WATER} for ${high_streak} polls -- provisioning an edge"
        if run_hook provision-edge "${REGION}"; then
            log "provision-edge hook ran; new node will appear once it heartbeats"
        fi
        high_streak=0
        sleep "${POLL_INTERVAL}"
        continue
    fi

    # --- scale in --------------------------------------------------------
    if [[ -n "${draining_id}" ]]; then
        nodes_json="$(fetch_nodes)" || { sleep "${POLL_INTERVAL}"; continue; }
        remaining="$(active_viewers_of "${nodes_json}" "${draining_id}")"
        remaining="${remaining:-0}"
        elapsed=$(( $(date -u +%s) - drain_started_at ))
        if [[ "${remaining}" -eq 0 || "${elapsed}" -ge "${DRAIN_TIMEOUT}" ]]; then
            [[ "${remaining}" -gt 0 ]] && log "drain timeout hit with ${remaining} viewers still on ${draining_id}; deprovisioning anyway"
            log "draining complete for ${draining_id} -- deprovisioning"
            if run_hook deprovision-edge "${draining_id}"; then
                remove_node "${draining_id}"
                log "removed ${draining_id} from the cluster table"
            else
                log "deprovision-edge hook failed or missing; leaving ${draining_id} drained but not removed"
                undrain_node "${draining_id}" # do not strand a node drained forever with no plan to remove it
            fi
            draining_id=""
        else
            log "waiting for ${draining_id} to drain (${remaining} viewers, ${elapsed}s/${DRAIN_TIMEOUT}s)"
        fi
        sleep "${POLL_INTERVAL}"
        continue
    fi

    if awk -v u="${utilization}" -v l="${LOW_WATER}" 'BEGIN{exit !(u<=l)}'; then
        low_streak=$((low_streak + 1))
    else
        low_streak=0
    fi

    if (( low_streak >= SCALE_IN_STREAK )) && (( healthy_edges > MIN_EDGES )); then
        nodes_json="$(fetch_nodes)" || { sleep "${POLL_INTERVAL}"; continue; }
        candidate="$(least_loaded_edge_id "${nodes_json}")"
        if [[ -n "${candidate}" ]]; then
            log "utilization <= ${LOW_WATER} for ${low_streak} polls with ${healthy_edges} healthy edges -- draining ${candidate}"
            drain_node "${candidate}"
            draining_id="${candidate}"
            drain_started_at="$(date -u +%s)"
        fi
        low_streak=0
    fi

    sleep "${POLL_INTERVAL}"
done
