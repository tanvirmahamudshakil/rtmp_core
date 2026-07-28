# Deployment

Phase 8 deployment deliverables (`docs/v2_promot.md` PHASE 8 "Deployment
tasks").

## Verification status — read this first

**Nothing in this document was executed in this repository's development
environment.** The project was developed on macOS, which has neither systemd,
nor logrotate, nor io_uring. The RTMP transport itself (`src/io/io_uring/`,
`apps/rtmp_server`) has never been compiled on this host — every phase report
since Phase 4 records this.

What that means concretely:

| Deliverable | Status |
|---|---|
| systemd unit | Written and reviewed line by line against `systemd.{unit,service,exec,kill,resource-control}(5)`. **Not loaded by a running systemd.** `systemd-analyze verify` was not available on this host. |
| Environment file | Written; syntax reviewed against `systemd.exec(5)`. Not loaded. |
| logrotate config | Written; reviewed against `logrotate(8)`. Not run. |
| Kernel tuning | Reasoning is from the Linux networking stack's documented behaviour. **Values not measured on your hardware** — see below. |
| Upgrade/rollback/backup/rotation procedures | Written from the code's actual behaviour. Not rehearsed end to end. |
| CMake presets, release gate, config validation | **Executed.** See `docs/phase-8-report.md`. |

Before first production use, work through "Pre-flight checklist" at the end.

## Building for production

```sh
cmake --preset production
cmake --build --preset production -j"$(nproc)"
```

The `production` preset (`CMakePresets.json`) sets:

- `CMAKE_BUILD_TYPE=RelWithDebInfo` — optimised, but debug info is retained so
  a core dump from production is analysable. A stripped `Release` build makes
  every production crash report useless.
- `RTMP_SERVER_BUILD_TESTS=OFF` — no test binaries in the artefact.
- `RTMP_SERVER_ENABLE_HARDENING=ON` — see below.

### Build hardening

`RTMP_SERVER_ENABLE_HARDENING` applies the flags in `cmake/Sanitizers.cmake`
(`rtmp_server_set_hardening`). Each, and why:

| Flag | Effect |
|---|---|
| `_FORTIFY_SOURCE=3` | Compile-time-sized checks on `memcpy`/`strcpy`/`sprintf` etc.; aborts instead of overflowing. Requires optimisation to have size information — this is why the preset is `RelWithDebInfo`, not `Debug`. |
| `-fstack-protector-strong` | Stack canary on any function with an array or an address-taken local. Turns a stack-buffer overflow into a controlled abort rather than a return-address overwrite. `strong` rather than `all` is the standard distribution trade-off. |
| `-D_GLIBCXX_ASSERTIONS` | Bounds checks on libstdc++ container `operator[]` and iterator arithmetic. No effect under libc++; harmless. |
| `-fno-delete-null-pointer-checks` | Stops the optimiser deleting a null check it "proved" redundant via UB — a recurring source of silently removed security checks. |
| `-fno-strict-aliasing` | This codebase reinterprets byte buffers as protocol structures; conservative aliasing keeps that sound. |
| `-Wl,-z,relro -Wl,-z,now` | Full RELRO: PLT/GOT resolved at load and made read-only, removing GOT overwrite as a technique. |
| `-Wl,-z,noexecstack` | Non-executable stack. |

Verify on the built binary:

```sh
checksec --file=/usr/local/bin/rtmp-server
# Expect: Full RELRO, Canary found, NX enabled, PIE enabled
```

### Sanitizer and debug builds

```sh
cmake --preset debug                             # unoptimised, assertions
cmake --preset asan  && ctest --preset asan      # ASan + UBSan
cmake --preset tsan  && ctest --preset tsan      # ThreadSanitizer
cmake --preset ubsan && ctest --preset ubsan     # UBSan alone (cheap, CI-friendly)
```

On a non-Linux host use the `-core-only` variant of each, which builds the
platform-independent subset.

**Sanitizer builds must never be deployed**: 2-20x slower, and ASan's
allocator is itself a memory-exhaustion risk under load.

### Release packaging

The artefact is one binary plus configuration:

```
/usr/local/bin/rtmp-server                    0755 root:root
/etc/rtmp-server/server.yaml                  0640 root:rtmp-server
/etc/rtmp-server/rtmp-server.env              0640 root:rtmp-server   # secrets
/etc/systemd/system/rtmp-server.service       0644 root:root
/var/lib/rtmp-server/                         0750 rtmp-server:rtmp-server
/usr/share/doc/rtmp-server/                   0644 root:root
```

Runtime dependencies (Debian/Ubuntu): `liburing2`, `libssl3`, `libsqlite3-0`.
Build: `liburing-dev`, `libssl-dev`, `libsqlite3-dev`, `cmake >= 3.25`,
`ninja-build`, a C++23 compiler.

**Reproducibility.** The build is reproducible given a pinned toolchain and
pinned dependency versions. It is *not* bit-for-bit reproducible out of the
box: `RelWithDebInfo` embeds absolute paths in debug info. Add
`-ffile-prefix-map=$(pwd)=/build` and build inside a fixed container image if
you need that. Record for every release: git commit SHA, compiler version,
dependency versions, and the exact configure line.

### One-command installer reset behaviour

`scripts/install-linux.sh` defaults to `RTMP_FRESH_INSTALL=1`. Before it
installs anything, it stops the previous StreamForge units and removes every
installer-owned binary, configuration file, database, stream key, recording,
backup, web asset, network-tuning file, system account and production build
artefact. Each removal is conditional, so a first install and a partially
removed install both continue without errors. The OS packages are not purged
because they may be shared by unrelated software; in fresh mode the installer
reinstalls existing required packages and installs any that are missing.

This reset is intentionally destructive. Use `RTMP_FRESH_INSTALL=0` only for
an in-place installation that must retain the existing database, recordings
and secrets.

### Release gate

No artefact ships without:

```sh
./scripts/release_gate.sh
```

It fails on any of: tests failing, sanitizer errors, missing required
configuration, database migration failure, insecure defaults, or first-party
compiler warnings. On a non-Linux host it says so and is explicitly **not**
sufficient to approve a release.

## Configuration

Copy `config/server.example.yaml` to `/etc/rtmp-server/server.yaml`; put
secrets in `/etc/rtmp-server/rtmp-server.env` (see
`deploy/systemd/rtmp-server.env.example`). Precedence: CLI > environment >
file > default.

`core::ServerConfig::validate()` is the authoritative list of what is
required, and it **fails startup** rather than warning. It rejects:

- secrets shorter than 32 characters, well-known placeholders (`CHANGE_ME`,
  `changeme`, `secret`, ...), zero-entropy secrets (all one character), and
  reuse of the same value for both secrets;
- `maximum_rtmp_message_size` of zero or above 64 MiB;
- zero GOP-cache or subscriber-queue bounds (zero reads as "unlimited");
- non-positive `idle_timeout`/`handshake_timeout`;
- an empty `database_connection`, or an empty `recording_directory` when
  recording is enabled.

Generate secrets with `openssl rand -hex 32` — independently for each.

## systemd

`deploy/systemd/rtmp-server.service`; install per that file's header comment.
The directives that matter operationally:

### Graceful shutdown

`main()` installs `SIGTERM`/`SIGINT` handlers that call `WorkerPool::stop()`.
The handler only sets state (async-signal-safe); the drain runs on each
worker's own thread inside `run()` (`docs/shutdown-model.md`). The unit sets
`KillSignal=SIGTERM`, `KillMode=mixed`, `TimeoutStopSec=30s`.

30 seconds is sized for the slowest thing shutdown must finish: recording
finalisation. `recording::AsyncFileSink` writes to `<path>.part`, then
`fsync`s and atomically renames into place, on a dedicated writer thread with
a bounded queue; draining a full 16 MiB queue to slow storage is the worst
case. Raise it if you record long sessions to network storage; lower it only
if you do not record at all.

```sh
systemctl stop rtmp-server               # graceful: drains, finalises recordings
systemctl kill -s SIGKILL rtmp-server    # ungraceful: .part files left behind
```

### Restart policy

`Restart=always` — a clean `exit(0)` from a network server is itself
unexpected. `RestartSec=2s` stops a crash loop saturating CPU and the log
pipeline. `StartLimitBurst=5` / `StartLimitIntervalSec=60s` gives up after 5
restarts in a minute, so a server that cannot stay up shows as a *failed*
unit rather than restarting invisibly for ever.

`RestartPreventExitStatus=1` is the important one: exit status 1 is what a
`ServerConfig::validate()` failure produces. Restarting cannot fix a bad
config, so the unit fails immediately and visibly.

### File descriptor limit

`LimitNOFILE=65536`. The reasoning, because a number without reasoning is
cargo cult:

| Consumer | Count at defaults |
|---|---|
| RTMP connections (`maximum_connections`) | 10 000 |
| Concurrent recording files (`maximum_publishers`) | 1 000 |
| io_uring rings/eventfds (per worker) | ~500 |
| Management API sockets, SQLite, logs | ~100 |
| Headroom for accept bursts and TIME_WAIT churn | ~2x |

≈ 12 000 steady state; 65536 leaves roughly 5x headroom.

**Keep this in step with `maximum_connections`.** The server enforces its own
connection limit and refuses excess connections cleanly. If it hits the fd
limit first, `accept()` returns `EMFILE`, which degrades *every* connection
instead of cleanly refusing new ones. The fd limit must always be the looser
of the two.

`LimitMEMLOCK=536870912` — io_uring registered buffers are pinned and count
against `RLIMIT_MEMLOCK`: `registered_buffer_count` × `registered_buffer_size`
× `worker_ring_count` = 1024 × 64 KiB × 8 = 512 MiB at defaults.

## Kernel tuning

**Do not apply these blindly.** Each entry states what it does, why it matters
for *this* workload, and what to watch. Modern distribution defaults are
reasonable; change a value only when a measurement says the default is the
constraint. Measure, change one thing, measure again.

Nothing here was measured on this project's development host — no Linux host
was available. The reasoning is from documented kernel behaviour; the numbers
are starting points, not answers.

### Connection acceptance

```sh
net.core.somaxconn = 8192            # completed-handshake queue, waiting for accept()
net.ipv4.tcp_max_syn_backlog = 8192  # half-open queue
```

**Why.** `listen_backlog` is capped by `somaxconn`; asking `listen()` for more
silently gets you `somaxconn`. When a stream goes viral, thousands of viewers
connect within seconds, and a full accept queue makes the kernel drop SYNs —
clients time out, retry, and amplify the burst.

**Expected effect.** Absorbs accept bursts. It does *not* make accepting
faster; it buys the accept loop time to catch up. A persistently full queue
means the accept loop is the bottleneck and a bigger queue only adds latency.

**Watch.** `ss -lnt` (`Send-Q` is the backlog limit, `Recv-Q` the current
depth); `nstat -az TcpExtListenOverflows` — any growth means dropped
connections.

```sh
net.ipv4.tcp_syncookies = 1
```

Usually already 1. Keep it: a public RTMP port is a SYN-flood target.

### Socket buffers

```sh
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
```

**Why.** Send buffers dominate: egress is the server's job. A buffer must hold
at least one bandwidth-delay product or the sender stalls waiting for ACKs.
For a 5 Mb/s stream to a viewer 100 ms away: 5 Mb/s × 0.1 s ≈ 62 KB. The 64 KB
default is *just* enough for that, and not enough for 20 Mb/s or a 250 ms path.

**Expected effect.** Fewer send stalls on high-bitrate or long-RTT viewers,
so fewer slow-viewer evictions from the bounded per-viewer queues.

**Cost, and why these are `max` values.** These are *ceilings for autotuning*,
not per-socket allocations — the kernel grows a buffer only as far as a socket
needs. At 10 000 connections a 16 MB ceiling is a theoretical 160 GB;
autotuning means you will never see that. What *would* realise it is disabling
autotuning (`net.ipv4.tcp_moderate_rcvbuf = 0`) or setting `SO_SNDBUF`
explicitly. **Do not set `SO_SNDBUF`** — it disables autotuning and locks in
the size.

**Watch.** `ss -tmi` for per-socket `wmem`/`rmem`; process RSS.

### Ephemeral ports

```sh
net.ipv4.ip_local_port_range = 10240 65535
```

**Why.** Only relevant to hosts making many *outbound* connections. A plain
ingest/egress server does not, so **this is usually unnecessary here.** It is
listed because it matters on the *proxy* host, where a single proxy→origin
address pair really can exhaust the ~28 000 default range, and if this server
ever pushes to an upstream CDN.

**Expected effect on this server in the common deployment: none.**

### TIME_WAIT

```sh
net.ipv4.tcp_fin_timeout = 30
```

**Why.** RTMP connections are long-lived, so TIME_WAIT accumulation matters
far less than for HTTP. This just bounds how long `FIN_WAIT_2` sockets linger
after a viewer disappears without a clean close.

**Do NOT set `net.ipv4.tcp_tw_reuse=1`, and do not go looking for
`tcp_tw_recycle`.** `tcp_tw_recycle` was removed in Linux 4.12 because it
broke NAT'd clients catastrophically. `tcp_tw_reuse` only affects outbound
connections and does nothing useful here. These two appear in every "TCP
tuning" blog post and are the most common way to break a production server
while believing you tuned it.

### Congestion control

```sh
net.core.default_qdisc = fq
net.ipv4.tcp_congestion_control = bbr
```

**Why.** Live streaming is sustained bulk transfer to many receivers. CUBIC
(the default) treats any loss as congestion and backs off hard, which on lossy
last-mile links (mobile, Wi-Fi) causes exactly the throughput collapse that
makes viewers rebuffer. BBR models bottleneck bandwidth and RTT rather than
using loss as the signal, and typically sustains materially higher throughput
on such paths.

**Expected effect.** Higher, steadier per-viewer throughput on lossy paths;
fewer slow-viewer evictions.

**Caveat, because BBR is not free.** BBR can be unfair to CUBIC flows sharing
a bottleneck, and BBRv1 under-performs on very shallow buffers. `fq` is
required — BBR depends on the pacing it provides. **A/B test this.** It is the
tunable most worth measuring and the one most likely to disappoint if adopted
on faith.

### io_uring

The unit's `LimitMEMLOCK` covers registered buffers. Note that on kernels
where io_uring is disabled by policy (`kernel.io_uring_disabled=2`, used by
some hardened distributions) the server **cannot start at all** — check this
first when a fresh host fails immediately.

### Applying and persisting

```sh
sudo tee /etc/sysctl.d/60-rtmp-server.conf >/dev/null <<'EOF'
net.core.somaxconn = 8192
net.ipv4.tcp_max_syn_backlog = 8192
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
net.core.default_qdisc = fq
net.ipv4.tcp_congestion_control = bbr
EOF
sudo sysctl --system
```

### NIC and bandwidth

Bandwidth, not CPU, is usually the binding constraint. 1 000 viewers of a
5 Mb/s stream is 5 Gb/s sustained egress — a 1 GbE interface saturates at
~200 viewers no matter how fast the server is. Size the interface from
`viewers × bitrate`, and put a CDN in front of HLS.

```sh
ethtool -G eth0 rx 4096 tx 4096        # larger rings reduce drops on bursts
ethtool -K eth0 gro on gso on tso on   # offloads: fewer per-packet CPU cycles
ethtool -l eth0                        # combined queues should be >= worker count
```

**Watch.** `ethtool -S eth0 | grep -Ei 'drop|err|discard'`; `ip -s link`.

### CPU affinity

`worker_cpu_pinning_enabled` pins worker *i* to CPU *i % nproc*. **Default
off, deliberately.** Pinning helps on dedicated bare metal with a stable
topology (cache locality, fewer migrations). It hurts on shared/cloud hosts,
where the hypervisor moves vCPUs underneath you and pinning fights the
scheduler. Enable only on dedicated hardware, and only after measuring.

### Core dumps

The unit sets `LimitCORE=infinity` because a crash in a media server is worth
analysing and the production build keeps debug info.

```sh
sudo sysctl -w kernel.core_pattern='|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h'
coredumpctl list rtmp-server
coredumpctl debug rtmp-server
```

**A core dump contains `token_signing_secret`, `api_authentication_secret`,
stream keys and in-flight media.** Treat cores as secrets: restrict
`/var/lib/systemd/coredump`, set a short retention in
`/etc/systemd/coredump.conf`, and set `LimitCORE=0` if you cannot protect them.

## Health checks

Two endpoints on the management port (Phase 5):

| Endpoint | Meaning | Drives |
|---|---|---|
| `/health/live` | Process is up and the event loop is responsive. | Restart decisions. |
| `/health/ready` | The above, plus dependencies (database) usable and accepting connections. | Load-balancer membership. |

The distinction matters: a server that is alive but not ready (opening the
database, or draining for shutdown) should be *removed from the load
balancer*, not *restarted*. Wiring both to the same action defeats the point.

```yaml
livenessProbe:
  httpGet: { path: /health/live, port: 8080 }
  periodSeconds: 10
  failureThreshold: 3            # 30s of failure before a restart
readinessProbe:
  httpGet: { path: /health/ready, port: 8080 }
  periodSeconds: 5
  failureThreshold: 2
terminationGracePeriodSeconds: 45   # must exceed TimeoutStopSec
```

systemd's `WatchdogSec` is deliberately **not** enabled in the unit: the
server does not call `sd_notify(WATCHDOG=1)`, so enabling it would have
systemd kill a perfectly healthy process every interval.

Prometheus scrapes `/metrics` on the same port. Both `/metrics` and `/api/`
must be unreachable from the internet (`docs/tls.md`).

## Upgrade procedure

The server is a single process with a single-writer SQLite database. There is
no in-place binary hot-reload and no clustering, so an upgrade is a restart,
and a restart drops connections: **publishers must reconnect and viewers
re-buffer.** Plan for that.

1. **Read the release notes** for schema and configuration changes.
2. **Back up** (below). Non-negotiable — this is the rollback point.
3. **Stage the new binary alongside the old**, do not overwrite:
   ```sh
   sudo install -m 0755 rtmp-server /usr/local/bin/rtmp-server.new
   /usr/local/bin/rtmp-server.new --version
   ```
4. **Validate the config against the new binary before cutting over.** Startup
   validation is strict and a release may add a required key. If your build
   has no `--validate-only`, start it against a throwaway config on unused
   ports and confirm it reaches "listening".
5. **Drain**: remove the node from the load balancer, wait for publishers to
   finish or migrate. `/health/ready` should go unhealthy first.
6. **Cut over**:
   ```sh
   sudo systemctl stop rtmp-server        # graceful; finalises recordings
   sudo mv /usr/local/bin/rtmp-server /usr/local/bin/rtmp-server.prev
   sudo mv /usr/local/bin/rtmp-server.new /usr/local/bin/rtmp-server
   sudo systemctl start rtmp-server
   ```
7. **Verify**:
   ```sh
   systemctl status rtmp-server
   curl -fsS localhost:8080/health/ready
   journalctl -u rtmp-server -n 100 --no-pager
   # then a real end-to-end publish + play
   ```
8. **Return to the load balancer**, and watch metrics for at least one full
   stream lifecycle before declaring success.

Multi-node: roll one node at a time, verifying each before proceeding.

With `RTMP_FRESH_INSTALL=0`, `scripts/install-linux.sh` automates the binary
part of this procedure. It hashes the production build, copies the current
executable to `/usr/local/bin/rtmp-server.previous`, installs the new
executable to a staged path, verifies the staged SHA-256, and only then
atomically renames it into place. A checksum mismatch leaves the running
installation unchanged. The default full-clean mode deliberately retains no
previous installation or rollback binary.

## Rollback procedure

Decide **before** upgrading what triggers a rollback (error rate, publish
failures, viewer disconnects), so the decision is not made under pressure.

**Binary only, no schema change:**

```sh
sudo systemctl stop rtmp-server
sudo mv /usr/local/bin/rtmp-server.prev /usr/local/bin/rtmp-server
sudo systemctl start rtmp-server
curl -fsS localhost:8080/health/ready
```

**With a schema change:** the database must go back too — a newer schema is
not readable by an older binary.

```sh
sudo systemctl stop rtmp-server
sudo mv /usr/local/bin/rtmp-server.prev /usr/local/bin/rtmp-server
sudo cp /var/lib/rtmp-server/rtmp.db /var/lib/rtmp-server/rtmp.db.failed-upgrade
sudo -u rtmp-server cp /var/backups/rtmp-server/rtmp-<timestamp>.db \
                       /var/lib/rtmp-server/rtmp.db
sudo systemctl start rtmp-server
```

**Everything created between the backup and the rollback is lost** — streams,
applications, rotated keys. Keep the upgrade window short, and keep
`rtmp.db.failed-upgrade` so the delta can be reconciled by hand.

Recordings are unaffected: they are independent `.flv` files, and any
in-progress recording was finalised by the graceful stop (or left as `.part`
if it was not).

Verify a rollback with a real publish and play, not just the health endpoint.

## Backup procedure

Two independent things to protect.

### 1. The SQLite database

`/var/lib/rtmp-server/rtmp.db` — applications, streams, hashed stream keys,
configuration state. Small, high value.

**Use `sqlite3 .backup`, not `cp`.** Copying a live SQLite file can capture a
torn page set and produce a database that opens but is subtly corrupt. The
backup API takes a consistent snapshot of a live database without stopping the
server.

```sh
#!/usr/bin/env bash
# /usr/local/bin/rtmp-server-backup
set -euo pipefail
DB=/var/lib/rtmp-server/rtmp.db
DEST=/var/backups/rtmp-server
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
install -d -m 0700 -o rtmp-server -g rtmp-server "$DEST"

sqlite3 "$DB" ".backup '$DEST/rtmp-$STAMP.db'"
# Prove the snapshot is readable before trusting it. An unverified backup is
# not a backup, it is a hypothesis.
sqlite3 "$DEST/rtmp-$STAMP.db" 'PRAGMA integrity_check;' | grep -qx ok
gzip -9 "$DEST/rtmp-$STAMP.db"
find "$DEST" -name 'rtmp-*.db.gz' -mtime +30 -delete
```

Schedule hourly with a systemd timer (`OnCalendar=hourly`, `Persistent=true`).

**The backup contains hashed stream keys.** Encrypt at rest; restrict
`/var/backups/rtmp-server` to 0700.

**Restore:**
```sh
sudo systemctl stop rtmp-server
sudo -u rtmp-server sh -c 'gunzip -c /var/backups/rtmp-server/rtmp-<stamp>.db.gz > /var/lib/rtmp-server/rtmp.db'
sudo -u rtmp-server sqlite3 /var/lib/rtmp-server/rtmp.db 'PRAGMA integrity_check;'
sudo systemctl start rtmp-server
```

**Rehearse the restore on a staging host.** A backup procedure that has never
been restored is untested code.

### 2. Recordings

`/var/lib/rtmp-server/recordings/` — large, append-only, individually
disposable. Sync completed files to object storage; never back up `.part`
files (in-progress, or orphaned by a hard kill).

```sh
rclone sync /var/lib/rtmp-server/recordings remote:rtmp-recordings \
    --exclude '*.part' --min-age 1h
```

`recording::retention` prunes old recordings by count/age. **Disk capacity is
the constraint that bites**: one 5 Mb/s stream is ~2.2 GB/hour, so 10
concurrent 24-hour recordings is ~528 GB/day. Alert on disk usage well before
full — a full disk fails recordings *and* the SQLite database.

### 3. Configuration

`server.yaml` in configuration management; `rtmp-server.env` in a secret
manager. **Never commit the env file to a repository.**

## Secret rotation

### `token_signing_secret`

Signs playback tokens. **Rotating it invalidates every outstanding signed
playback URL** — viewers holding a valid unexpired token are rejected on
reconnect and must fetch a new one.

The server holds a single signing secret with no overlap window, so rotation
is a hard cut. To minimise impact:

1. Shorten token expiry to minutes ahead of the rotation so the outstanding
   set drains.
2. Wait for the longest previously-issued expiry to pass.
3. Rotate:
   ```sh
   NEW=$(openssl rand -hex 32)
   sudo sed -i "s|^RTMP_SERVER_TOKEN_SIGNING_SECRET=.*|RTMP_SERVER_TOKEN_SIGNING_SECRET=$NEW|" \
       /etc/rtmp-server/rtmp-server.env
   sudo systemctl restart rtmp-server
   ```
4. Re-issue tokens from the management API.

If you cannot tolerate a hard cut, the server needs support for a *previous*
secret accepted during an overlap window. It does not have that today —
recorded as an open gap in `docs/production-readiness.md`.

Rotate quarterly, and immediately on any suspected exposure.

### `api_authentication_secret`

Authenticates management API callers. Rotation breaks in-flight API clients
but never affects publishers or viewers, so it is much cheaper.

```sh
NEW=$(openssl rand -hex 32)
sudo sed -i "s|^RTMP_SERVER_API_AUTHENTICATION_SECRET=.*|RTMP_SERVER_API_AUTHENTICATION_SECRET=$NEW|" \
    /etc/rtmp-server/rtmp-server.env
sudo systemctl restart rtmp-server
# then update every API client
```

Must differ from `token_signing_secret` — `validate()` enforces this, because
reuse means a leaked management credential also forges playback tokens.

### Per-stream publish keys

Rotated individually through the management API, without a restart and
without affecting other streams:

```sh
curl -fsS -X POST -H "Authorization: Bearer $API_SECRET" \
     localhost:8080/api/streams/<id>/rotate-key
```

The raw key is returned **once** and never persisted (only its hash is). The
publisher must be updated; an existing publish session continues until it
disconnects. Rotate immediately if a key was shared, committed, or logged.

### TLS certificates

Owned by the proxy (`docs/tls.md`). Automate with ACME; alert at 21 days
remaining.

## Monitoring

`/metrics` (Prometheus format) on the management port. Alert on:

| Signal | Why |
|---|---|
| `authentication_failures` rate | Credential stuffing, or a misconfigured publisher. |
| Slow-viewer evictions | Egress bandwidth or socket buffers are the constraint. |
| Recording queue depth / `dropped_frames` | Storage cannot keep up; recordings are lossy. |
| `active_connections` vs `maximum_connections` | Approaching the cap; connections will be refused. |
| Disk usage on `/var/lib/rtmp-server` | A full disk fails recordings *and* the database. |
| Unit restart count | `systemctl show -p NRestarts rtmp-server`. |

Logs are structured JSON on stdout, collected by the journal; retention is set
in `journald.conf`. `deploy/logrotate/rtmp-server` covers the file-redirect
alternative and explains why it needs `copytruncate`.

## Pre-flight checklist

Before the first production deployment, **on the target Linux host**:

- [ ] `systemd-analyze verify /etc/systemd/system/rtmp-server.service` — clean
- [ ] `systemd-analyze security rtmp-server` — review the exposure score
- [ ] `./scripts/release_gate.sh` on Linux — passes, *including* the io_uring
      targets that cannot be built on macOS
- [ ] `checksec --file=/usr/local/bin/rtmp-server` — Full RELRO, canary, NX, PIE
- [ ] `systemctl start` then `systemctl stop` — a graceful drain finalises
      recordings (no stray `.part` files)
- [ ] `kill -9` the process — systemd restarts it and it recovers
- [ ] Corrupt the config — the unit fails and does **not** crash-loop
- [ ] Back up, restore onto staging, then publish and play
- [ ] `nmap -p 1935,8080 <public-address>` from outside — plaintext ports
      unreachable
- [ ] `testssl.sh --full <host>:443` — TLS as specified in `docs/tls.md`
- [ ] Per-IP rate limiting: either PROXY protocol support exists, or
      equivalent limits are enforced at the proxy (`docs/tls.md`)
- [ ] End-to-end publish from OBS and play from a real player

## Related

- `docs/tls.md` — TLS termination strategy and proxy configuration
- `docs/security.md` — security posture and findings
- `docs/production-readiness.md` — acceptance criteria status and open gaps
- `docs/shutdown-model.md` — graceful shutdown internals
- `docs/observability.md` — metrics and structured logging
- `docs/configuration.md` — every configuration key
