#!/usr/bin/env bash
# Example on-promote hook: point the deployment's DNS record at this node
# once it becomes leader. ha-agent.sh runs this (if executable) right after
# rtmp-server and litestream are up, at /etc/streamforge/ha-hooks/on-promote.
#
# This is ONE example (Cloudflare's API) among many equally valid approaches
# — a floating IP API call (Hetzner/DigitalOcean/AWS EIP re-association), a
# keepalived VRRP priority change, or an internal load balancer's backend
# re-registration all fit the same hook point. Pick whichever matches your
# network; StreamForge has no opinion here, which is why this ships as an
# example, not a default.
#
# A hook failure is logged by ha-agent.sh but never blocks failover: an
# origin that is correctly promoted and serving, but not yet reachable at the
# old DNS name, is a strictly better outcome than refusing to fail over
# because a DNS API call timed out.
set -Eeuo pipefail

: "${CLOUDFLARE_API_TOKEN:?}"
: "${CLOUDFLARE_ZONE_ID:?}"
: "${CLOUDFLARE_RECORD_ID:?}"
: "${STREAMFORGE_PUBLIC_HOSTNAME:?}"
: "${STREAMFORGE_PUBLIC_IP:?}"  # this node's own public IP

curl -sS -X PATCH \
  "https://api.cloudflare.com/client/v4/zones/${CLOUDFLARE_ZONE_ID}/dns_records/${CLOUDFLARE_RECORD_ID}" \
  -H "Authorization: Bearer ${CLOUDFLARE_API_TOKEN}" \
  -H "Content-Type: application/json" \
  --data "{\"type\":\"A\",\"name\":\"${STREAMFORGE_PUBLIC_HOSTNAME}\",\"content\":\"${STREAMFORGE_PUBLIC_IP}\",\"ttl\":60,\"proxied\":false}"
