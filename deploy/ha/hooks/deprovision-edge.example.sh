#!/usr/bin/env bash
# Example deprovision-edge hook: the "warm pool" counterpart to
# provision-edge.example.sh. autoscaler.sh calls this with the node id (the
# same id the node heartbeats with) as $1, only after that node has already
# been drained (active_viewers reached 0, or the drain timeout elapsed) and
# autoscaler.sh is about to DELETE it from the cluster table.
#
# For a warm pool, "deprovisioning" means returning the machine to the idle
# pool rather than destroying it: disable its heartbeat timer so it stops
# reporting itself as an edge, but leave Caddy/Varnish installed for the next
# provision-edge call. A true elastic deployment's version of this hook would
# instead terminate the VM via its cloud API.
#
# The node id is the STREAMFORGE_NODE_ID it heartbeats with, which
# install-edge.sh derives from `hostname -s` by default -- adjust the mapping
# below if your deployment sets STREAMFORGE_NODE_ID to something else.
set -Eeuo pipefail

node_id="${1:?node id required}"

echo "returning ${node_id} to the warm pool"
ssh -o ConnectTimeout=5 -o BatchMode=yes "root@${node_id}" \
    'systemctl disable --now streamforge-node-heartbeat.timer'
