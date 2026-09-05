#!/usr/bin/env bash
# StreamForge HA agent: lease-based leader election and automatic origin
# failover, built entirely on the same S3-compatible bucket litestream
# already replicates the control-plane database to. No extra infrastructure
# (no etcd/Consul/Postgres cluster) — the trade-off, stated plainly, is in
# docs/high-availability.md "What this is not". Read that before deploying
# this in front of anything that matters.
#
# Model: exactly one candidate origin is ever the writer (the "leader"). It
# holds a time-limited lease object in the bucket and renews it well before
# expiry. Every other candidate is a passive standby: rtmp_server and
# litestream-replicate are stopped, nothing is written locally. The instant a
# standby observes the lease has expired, it attempts to take it over; if it
# succeeds, it restores the database fresh from the latest replicated
# snapshot (litestream's own documented failover pattern) and starts serving.
#
# Split-brain safety: a leader that fails to renew its lease before expiry
# self-fences — it stops rtmp_server and litestream-replicate immediately,
# on the assumption that if IT cannot reach the bucket, a standby may already
# be taking over. This is best-effort (it depends on synchronized clocks
# across nodes — run NTP/chrony — and on the object store honouring
# conditional writes correctly) and is not a substitute for a real consensus
# system. It is the same trade-off every lease-based HA tool built without a
# dedicated consensus store makes.
#
# Requires: aws-cli (or any S3-compatible CLI that supports conditional
# writes — --if-none-match / --if-match on put-object; MinIO's `mc` supports
# this from 2024 releases, as do AWS S3, Cloudflare R2 and Backblaze B2). If
# your object store does not support conditional writes, this script's
# lease acquisition is not safe against a race between two standbys and you
# should use a real consensus store (etcd/Consul) instead — see
# docs/high-availability.md.
#
# Install as streamforge-ha-agent.service (see the unit at the bottom of this
# file's companion, deploy/ha/streamforge-ha-agent.service) on every candidate
# origin. Exactly one of them should start as leader on first bring-up
# (STREAMFORGE_HA_BOOTSTRAP_LEADER=1 on that one only, one time); every
# subsequent leadership change is fully automatic.
#
# Environment:
#   STREAMFORGE_HA_NODE_ID        (required) unique id for this candidate,
#                                 e.g. origin-a. Same value litestream.yml's
#                                 host would use; not related to cluster node
#                                 registry ids.
#   STREAMFORGE_HA_BUCKET         (required) same bucket litestream.yml uses.
#   STREAMFORGE_HA_S3_ENDPOINT    S3-compatible endpoint URL (empty = AWS S3).
#   STREAMFORGE_HA_S3_REGION      default us-east-1.
#   STREAMFORGE_HA_LEASE_KEY      object key for the lease. Default
#                                 "rtmp-db/ha-lease.json".
#   STREAMFORGE_HA_LEASE_TTL      seconds before an unrenewed lease is
#                                 considered expired. Default 15.
#   STREAMFORGE_HA_RENEW_INTERVAL seconds between renewals while leader.
#                                 Default 5 (three renewals per TTL window,
#                                 so one or two missed ticks from transient
#                                 network jitter do not trigger a failover).
#   STREAMFORGE_HA_POLL_INTERVAL  seconds between standby lease checks.
#                                 Default 3.
#   STREAMFORGE_HA_DB_PATH        default /var/lib/rtmp-server/rtmp.db.
#   STREAMFORGE_HA_RTMP_SERVICE   systemd unit to start/stop for leadership.
#                                 Default rtmp-server.
#   STREAMFORGE_HA_LITESTREAM_SERVICE  default litestream.
#   STREAMFORGE_HA_HOOKS_DIR      directory for optional on-promote/on-demote
#                                 executable hooks (VIP/DNS flip -- see
#                                 docs/high-availability.md for an example).
#                                 Default /etc/streamforge/ha-hooks.
#   STREAMFORGE_HA_BOOTSTRAP_LEADER  "1" on exactly one node's very first
#                                 start, when the lease object does not exist
#                                 yet and no database has ever been written.
set -Eeuo pipefail
IFS=$'\n\t'
PATH=/usr/sbin:/usr/bin:/sbin:/bin

NODE_ID="${STREAMFORGE_HA_NODE_ID:?STREAMFORGE_HA_NODE_ID is required}"
BUCKET="${STREAMFORGE_HA_BUCKET:?STREAMFORGE_HA_BUCKET is required}"
ENDPOINT_ARGS=()
[[ -n "${STREAMFORGE_HA_S3_ENDPOINT:-}" ]] && ENDPOINT_ARGS=(--endpoint-url "${STREAMFORGE_HA_S3_ENDPOINT}")
REGION="${STREAMFORGE_HA_S3_REGION:-us-east-1}"
LEASE_KEY="${STREAMFORGE_HA_LEASE_KEY:-rtmp-db/ha-lease.json}"
LEASE_TTL="${STREAMFORGE_HA_LEASE_TTL:-15}"
RENEW_INTERVAL="${STREAMFORGE_HA_RENEW_INTERVAL:-5}"
POLL_INTERVAL="${STREAMFORGE_HA_POLL_INTERVAL:-3}"
DB_PATH="${STREAMFORGE_HA_DB_PATH:-/var/lib/rtmp-server/rtmp.db}"
RTMP_SERVICE="${STREAMFORGE_HA_RTMP_SERVICE:-rtmp-server}"
LITESTREAM_SERVICE="${STREAMFORGE_HA_LITESTREAM_SERVICE:-litestream}"
HOOKS_DIR="${STREAMFORGE_HA_HOOKS_DIR:-/etc/streamforge/ha-hooks}"
BOOTSTRAP="${STREAMFORGE_HA_BOOTSTRAP_LEADER:-0}"

LEASE_TMP="$(mktemp)"
trap 'rm -f "${LEASE_TMP}"' EXIT

log() { printf '%s streamforge-ha-agent[%s]: %s\n' "$(date -u +%FT%TZ)" "${NODE_ID}" "$*"; }
die() { log "FATAL: $*"; exit 1; }

s3() { aws s3api "${ENDPOINT_ARGS[@]}" --region "${REGION}" "$@"; }

run_hook() {
    local hook="${HOOKS_DIR}/$1"
    if [[ -x "${hook}" ]]; then
        log "running hook $1"
        "${hook}" || log "hook $1 exited non-zero (continuing -- a VIP/DNS hook failure must not block failover)"
    fi
}

now_epoch() { date -u +%s; }

# Reads the current lease. Sets LEASE_ETAG (empty if absent), LEASE_LEADER,
# LEASE_EXPIRES (0 if absent/unparseable).
read_lease() {
    LEASE_ETAG=""
    LEASE_LEADER=""
    LEASE_EXPIRES=0
    local head
    if ! head="$(s3 head-object --bucket "${BUCKET}" --key "${LEASE_KEY}" 2>/dev/null)"; then
        return 0 # no lease object yet
    fi
    LEASE_ETAG="$(printf '%s' "${head}" | grep -o '"ETag": *"[^"]*"' | sed 's/.*"\(.*\)"$/\1/')"
    if s3 get-object --bucket "${BUCKET}" --key "${LEASE_KEY}" "${LEASE_TMP}" >/dev/null 2>&1; then
        LEASE_LEADER="$(grep -o '"leader_id" *: *"[^"]*"' "${LEASE_TMP}" | sed 's/.*"\(.*\)"$/\1/')"
        LEASE_EXPIRES="$(grep -o '"expires_at" *: *[0-9]*' "${LEASE_TMP}" | grep -o '[0-9]*$')"
        LEASE_EXPIRES="${LEASE_EXPIRES:-0}"
    fi
}

# Attempts to atomically write the lease. `condition` is either
# "--if-none-match *" (create only if absent) or "--if-match <etag>" (replace
# only if unchanged since we last read it). Returns 0 only if the write is
# confirmed accepted by the store -- a rejected conditional write (someone
# else already holds or took the lease) returns non-zero, and the caller must
# treat that as "I am not the leader", not retry blindly.
write_lease() {
    local -a condition=("$@")
    local expires
    expires=$(( $(now_epoch) + LEASE_TTL ))
    local body
    body=$(printf '{"leader_id":"%s","acquired_at":%s,"expires_at":%s}' \
        "${NODE_ID}" "$(now_epoch)" "${expires}")
    printf '%s' "${body}" > "${LEASE_TMP}"
    s3 put-object --bucket "${BUCKET}" --key "${LEASE_KEY}" --body "${LEASE_TMP}" \
        --content-type application/json "${condition[@]}" >/dev/null 2>&1
}

promote_to_leader() {
    log "promoting to leader"
    run_hook "pre-promote"
    systemctl stop "${LITESTREAM_SERVICE}" 2>/dev/null || true
    systemctl stop "${RTMP_SERVICE}" 2>/dev/null || true

    # Always restore fresh: this is what makes promotion safe even after this
    # node was standby for hours -- its local copy (if any) may be stale or,
    # worse, may itself contain writes from a previous, since-fenced stint as
    # leader (split-brain aftermath). The replica in the bucket is the only
    # copy trusted once this node is about to become the sole writer.
    rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm"
    if ! litestream restore -o "${DB_PATH}" "s3://${BUCKET}/rtmp-db" ${STREAMFORGE_HA_S3_ENDPOINT:+-endpoint "${STREAMFORGE_HA_S3_ENDPOINT}"}; then
        if [[ "${BOOTSTRAP}" == "1" && ! -e "${DB_PATH}" ]]; then
            log "no replica exists yet; proceeding with a fresh database (bootstrap)"
        else
            die "litestream restore failed and no replica exists -- refusing to start as leader with no database"
        fi
    fi

    systemctl start "${RTMP_SERVICE}" || die "rtmp-server failed to start after promotion"
    systemctl start "${LITESTREAM_SERVICE}" || log "litestream failed to start -- this leader is now UNREPLICATED, fix immediately"
    run_hook "on-promote"
    log "promotion complete; serving as leader"
}

demote_to_standby() {
    log "demoting to standby"
    run_hook "pre-demote"
    # Order matters: stop the writer before stopping replication, so the last
    # few transactions (if any snuck in during a slow shutdown) still ship.
    systemctl stop "${RTMP_SERVICE}" 2>/dev/null || true
    systemctl stop "${LITESTREAM_SERVICE}" 2>/dev/null || true
    run_hook "on-demote"
}

STATE="standby"
[[ "${BOOTSTRAP}" == "1" ]] && STATE="bootstrapping"

log "starting (bucket=${BUCKET} lease-key=${LEASE_KEY} ttl=${LEASE_TTL}s)"

while true; do
    read_lease

    if [[ "${STATE}" == "leader" ]]; then
        # Renew well before expiry. A failed renewal is treated as a fencing
        # event, not a retry-later condition: this node cannot prove it still
        # holds the lease, so it must stop writing before someone else can.
        if [[ -n "${LEASE_LEADER}" && "${LEASE_LEADER}" != "${NODE_ID}" ]]; then
            log "SPLIT-BRAIN GUARD: lease is held by '${LEASE_LEADER}', not us -- demoting immediately"
            demote_to_standby
            STATE="standby"
        elif write_lease --if-match "${LEASE_ETAG}"; then
            : # renewed
        else
            log "lease renewal failed (network partition, or the lease was rewritten) -- self-fencing"
            demote_to_standby
            STATE="standby"
        fi
        sleep "${RENEW_INTERVAL}"
        continue
    fi

    # Standby (or bootstrapping): is the lease free to take?
    now="$(now_epoch)"
    if [[ -z "${LEASE_ETAG}" ]]; then
        if [[ "${STATE}" == "bootstrapping" ]]; then
            if write_lease --if-none-match '*'; then
                promote_to_leader
                STATE="leader"
                continue
            fi
            log "another node created the lease first; joining as standby"
            STATE="standby"
        fi
        # No lease and not bootstrapping: a fresh deployment where nobody has
        # bootstrapped yet, or every prior leader's lease already expired and
        # was never reclaimed (rare). Do not seize it non-interactively here --
        # an operator must bootstrap explicitly once, exactly as the top-of-
        # file comment says, so an empty bucket never silently elects a
        # leader with no data to restore.
    elif (( now >= LEASE_EXPIRES )); then
        log "lease held by '${LEASE_LEADER}' expired $((now - LEASE_EXPIRES))s ago -- attempting takeover"
        if write_lease --if-match "${LEASE_ETAG}"; then
            promote_to_leader
            STATE="leader"
            continue
        fi
        log "takeover lost the race to another standby"
    fi

    sleep "${POLL_INTERVAL}"
done
