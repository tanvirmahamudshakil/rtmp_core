# TLS strategy

Phase 8 security tasks 1 and 2 (`docs/v2_promot.md`): "add TLS strategy —
native RTMPS or documented TLS proxy termination" and "set secure protocol
and cipher defaults".

## Decision

**TLS is terminated by a reverse proxy in front of the server. Native RTMPS
is not implemented.**

The server speaks plaintext RTMP on its ingest port and plaintext HTTP on its
management/HLS port, and expects both to be reachable only from the proxy (or
from localhost), never from the internet.

## Why, and what native RTMPS would have cost

This is a real trade-off, not a shortcut. Both options were weighed:

### What native RTMPS would require

RTMPS is not a protocol variant — it is ordinary RTMP inside a TLS record
layer. Implementing it means inserting a TLS state machine between the socket
and `protocol::handshake::HandshakeSession`, and that insertion point is
`src/io/io_uring/`:

1. **It is not a drop-in wrapper.** The transport is completion-based
   io_uring, not readiness-based sockets. OpenSSL's BIO model is
   readiness-oriented; marrying it to `io_uring` means driving
   `SSL_read`/`SSL_write` against memory BIOs and manually pumping the
   resulting ciphertext through the existing submission/completion path.
   Every buffer-ownership rule the project established in Phases 0-4
   (registered buffers, provided buffer rings, zero-copy sends) has to be
   re-derived for a codepath where the bytes on the wire are no longer the
   bytes the protocol layer sees.
2. **It breaks the zero-copy fan-out invariant.** `SharedMediaFrame` exists so
   one immutable payload is written to N viewers with no per-viewer copy
   (rule 3.8). TLS encrypts per connection with a per-connection key, so every
   viewer needs its own ciphertext. Native RTMPS would reintroduce exactly the
   per-viewer copy the architecture was built to eliminate — and would do the
   encryption on the event-loop thread, which is CPU-heavy work on a thread
   that rule 3.6 says must not stall.
3. **It cannot be verified here.** `src/io/io_uring/` and `apps/rtmp_server`
   do not compile on this project's development host at all (macOS; io_uring
   is Linux-only — documented in every phase report since Phase 4). Native
   RTMPS would be several hundred lines of security-critical, buffer-handling
   code in the one part of the tree that cannot be built, run, sanitized or
   fuzzed here. Shipping unverified TLS code and calling it done is precisely
   what `docs/v2_promot.md` section 3.1 forbids.

### What the proxy gives instead

1. **Client support is the same or better.** RTMPS support in encoders is
   patchy: OBS supports it, many hardware encoders do not. The clients that do
   support RTMPS connect to a proxy identically — they cannot tell where TLS
   terminates.
2. **The TLS implementation is one that is actually maintained.** nginx,
   HAProxy and Envoy TLS stacks get CVE response, protocol updates and
   large-scale production exposure that a bespoke in-server implementation
   would not.
3. **Certificate lifecycle is solved.** ACME renewal, OCSP stapling, SNI and
   hot certificate reload are proxy features. Native RTMPS would need all of
   them re-implemented, including reloading a certificate without dropping
   in-flight streams.
4. **It offloads encryption from the media path.** The proxy encrypts on its
   own threads; the server keeps its shared-immutable-payload fan-out intact.
5. **It is verifiable.** The proxy configurations below are declarative and
   independently testable with `testssl.sh`/`openssl s_client` against a real
   deployment.

### What is given up

Honest statement of the cost:

- **The proxy-to-server hop is plaintext.** This is only acceptable when that
  hop does not cross a trust boundary: same host over loopback, or a private
  network segment you control. Over any shared or untrusted network it must
  be tunnelled (WireGuard/IPsec) or the proxy must be moved onto the server
  host. This is the single most important operational constraint in this
  document.
- **The client IP seen by the server is the proxy's** unless PROXY protocol is
  enabled. This matters: `RtmpAuthenticator`'s per-IP connection limits and
  authentication-failure lockout are keyed on client IP, and without PROXY
  protocol every connection appears to come from one address, which both
  defeats the lockout and lets one abusive client exhaust the per-IP limit for
  everyone. See "PROXY protocol" below — **this is required, not optional.**
- **There is no end-to-end guarantee**, only proxy-to-client. Anyone with
  access to the internal segment sees plaintext stream keys.

## Reconsider this decision if

- End-to-end encryption to the process becomes a compliance requirement, or
- the proxy-to-server hop cannot be kept inside a trust boundary, or
- a Linux CI host exists where a native implementation can be built, fuzzed
  and sanitized to the same standard as the rest of the tree.

The `network::TcpConnection` / `network::AsyncTransport` abstractions are the
seam a future native implementation would slot into: a `TlsTransport`
decorator implementing `AsyncTransport` over an inner transport, owning an
`SSL*` and two memory BIOs, would leave the protocol layer unchanged. That is
a design note, not an implementation — no such type exists in this tree.

## Required TLS configuration for the proxy

These are the "secure protocol and cipher defaults" of Phase 8 task 2, applied
at the layer that actually terminates TLS.

### Protocol versions

- **Enable TLS 1.3 and TLS 1.2. Nothing else.**
- TLS 1.0 and 1.1 are deprecated by RFC 8996 and rely on SHA-1/MD5 in the
  PRF and on CBC constructions with a long history of padding-oracle attacks.
- SSLv2/SSLv3 must be off (DROWN, POODLE).
- TLS 1.2 is retained only for older hardware encoders. Drop to TLS 1.3 only
  if you have verified every client supports it.

### Cipher suites

TLS 1.3 (suite selection is not negotiable in the same way; these three are
the whole set worth enabling):

```
TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256
```

TLS 1.2 — forward secrecy and AEAD only:

```
ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:
ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:
ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256
```

Every excluded suite is excluded for a reason:

| Excluded | Reason |
|---|---|
| Static RSA key exchange (`AES256-SHA` etc.) | No forward secrecy: one compromised private key retroactively decrypts all captured traffic. |
| CBC suites (`...-SHA`, `...-SHA256`) | Lucky13 and padding-oracle family; MAC-then-encrypt. |
| 3DES | 64-bit block size; Sweet32 birthday attack on long-lived connections — and RTMP connections are exactly that. |
| RC4 | Biased keystream, practically broken. |
| Anything `NULL`/`EXPORT`/`anon` | No encryption or no authentication. |
| DHE (finite-field) | Weak-parameter and Logjam risk; ECDHE is faster and safer. |

Additional settings:

- **Curves**: `X25519:prime256v1:secp384r1`.
- **Server cipher preference**: honour server order for TLS 1.2 so a client
  cannot steer to the weakest mutually supported suite.
- **Session tickets**: enable, but rotate keys frequently (nginx default key
  lifetime undermines forward secrecy if a ticket key is ever leaked).
- **OCSP stapling**: enable, so clients need not contact the CA.
- **HSTS**: on the HTTP/HLS listener only — meaningless for RTMPS.

### Certificates

- RSA 2048-bit minimum (prefer ECDSA P-256: smaller, faster handshakes,
  which matters when thousands of viewers connect at once).
- Automated renewal (ACME) with alerting at 21 days remaining.
- Serve the full chain; a missing intermediate fails on clients without
  AIA-chasing, which includes many embedded encoders.

## Reference: nginx (stream module, RTMPS)

`ngx_stream_ssl_module` terminates TLS and forwards plaintext RTMP.

```nginx
stream {
    # PROXY protocol carries the real client address to the server. See the
    # PROXY protocol section below - this is required for per-IP rate
    # limiting to function.
    server {
        listen 443 ssl;                  # RTMPS
        proxy_pass 127.0.0.1:1935;       # plaintext RTMP, loopback only
        proxy_protocol on;

        ssl_certificate     /etc/ssl/certs/stream.example.com.fullchain.pem;
        ssl_certificate_key /etc/ssl/private/stream.example.com.key;

        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_ciphers 'ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256';
        ssl_prefer_server_ciphers on;
        ssl_ecdh_curve X25519:prime256v1:secp384r1;

        ssl_session_cache shared:RTMPS:10m;
        ssl_session_timeout 10m;
        ssl_session_tickets on;

        # A stalled TLS handshake must not hold a worker slot.
        ssl_handshake_timeout 10s;
        proxy_timeout 60s;
    }
}
```

HTTPS for HLS and the management API:

```nginx
http {
    server {
        listen 443 ssl http2;
        server_name stream.example.com;

        ssl_certificate     /etc/ssl/certs/stream.example.com.fullchain.pem;
        ssl_certificate_key /etc/ssl/private/stream.example.com.key;
        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_ciphers 'ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256';
        ssl_prefer_server_ciphers on;
        ssl_stapling on;
        ssl_stapling_verify on;

        add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;

        # HLS playback: public.
        location /hls/ {
            proxy_pass http://127.0.0.1:8080;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        }

        # Health checks: public, no authentication, no sensitive data.
        location ~ ^/health/(live|ready)$ {
            proxy_pass http://127.0.0.1:8080;
            access_log off;
        }

        # Management API and metrics: NEVER expose these publicly. They can
        # rotate stream keys and disconnect publishers. Restrict by source
        # address at minimum; prefer not proxying them at all and reaching
        # them over an SSH tunnel or a private admin network.
        location /api/ {
            allow 10.0.0.0/8;
            deny all;
            proxy_pass http://127.0.0.1:8080;
        }
        location /metrics {
            allow 10.0.0.0/8;
            deny all;
            proxy_pass http://127.0.0.1:8080;
        }
    }
}
```

## Reference: HAProxy

```
global
    ssl-default-bind-options ssl-min-ver TLSv1.2 no-tls-tickets
    ssl-default-bind-ciphers ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256
    ssl-default-bind-ciphersuites TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256
    tune.ssl.default-dh-param 2048

frontend rtmps
    mode tcp
    bind :443 ssl crt /etc/ssl/private/stream.example.com.pem
    default_backend rtmp_origin

backend rtmp_origin
    mode tcp
    # send-proxy-v2 preserves the client address; see below.
    server origin 127.0.0.1:1935 send-proxy-v2 check
```

## PROXY protocol — required, not optional

`authentication::RtmpAuthenticator` enforces `max_connections_per_ip` and the
authentication-failure lockout **keyed on client IP**. With TLS terminated at
a proxy and no PROXY protocol, every connection arrives from the proxy's
address, and both mechanisms fail in the worst possible way: the per-IP
connection cap is consumed globally by the proxy's single address, and one
attacker's failed authentications lock out every legitimate client behind the
same proxy.

Therefore, one of these must hold:

1. The proxy sends PROXY protocol v2 (`proxy_protocol on` / `send-proxy-v2`)
   **and** the server parses it to recover the client address, **or**
2. per-IP limits are understood to be non-functional and an equivalent control
   is enforced at the proxy (nginx `limit_conn`, HAProxy `stick-table`).

**Current status: the server does not parse the PROXY protocol header.** No
PROXY protocol parsing exists in `src/io/io_uring/` or `src/protocol/`. Until
it does, option 2 applies: enforce connection and rate limits at the proxy,
and treat the server's per-IP limits as a second line of defence that is
currently ineffective behind a proxy. This is recorded as an open gap in
`docs/production-readiness.md`.

Note that a PROXY protocol header prepended to the RTMP stream would be
consumed by `HandshakeSession` as a malformed C0 and rejected, so enabling it
against the current server breaks ingest — do not enable `proxy_protocol on`
until parsing exists.

## Firewall

The proxy-termination model is only sound if the plaintext ports are actually
unreachable. Bind them to loopback (`RTMP_SERVER_API_BIND_ADDRESS=127.0.0.1`)
and enforce it at the packet filter too:

```
# Public: TLS only.
-A INPUT -p tcp --dport 443 -j ACCEPT

# Plaintext RTMP: only from the proxy host.
-A INPUT -p tcp --dport 1935 -s <proxy-address> -j ACCEPT
-A INPUT -p tcp --dport 1935 -j DROP

# Management API: loopback only.
-A INPUT -p tcp --dport 8080 -i lo -j ACCEPT
-A INPUT -p tcp --dport 8080 -j DROP
```

## Verifying a deployment

None of the above was executed in this repository's development environment.
Verify on the target host:

```sh
# Protocol versions: TLS 1.2/1.3 must connect, 1.0/1.1 must not.
openssl s_client -connect stream.example.com:443 -tls1_3 </dev/null
openssl s_client -connect stream.example.com:443 -tls1_2 </dev/null
openssl s_client -connect stream.example.com:443 -tls1_1 </dev/null   # must fail
openssl s_client -connect stream.example.com:443 -tls1   </dev/null   # must fail

# Full audit: protocols, suites, chain, stapling, known vulnerabilities.
testssl.sh --full stream.example.com:443

# The plaintext ports must NOT be reachable from outside.
nmap -p 1935,8080 <public-address>       # expect filtered/closed
```

## Related

- `docs/security.md` — overall security posture
- `docs/deployment.md` — systemd unit, firewall, secret rotation
- `docs/production-readiness.md` — acceptance criteria status, open gaps
- `docs/management-api.md` — why the management API must not be public
