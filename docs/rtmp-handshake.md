# RTMP Handshake

> Status: implemented in Phase 2. See `docs/phase2-checklist.md` for the phase
> tracking record.

## Scope

The RTMP "simple handshake" (plain RTMP, not RTMPS) that every connection
performs before any chunked RTMP traffic (chunk stream, AMF0, commands,
media) can flow. Implemented by
`rtmp_server::protocol::handshake::HandshakeSession`
(`include/rtmp_server/protocol/handshake/handshake_session.hpp`,
`src/protocol/handshake/handshake_session.cpp`).

## Message formats

All sizes are fixed by the protocol.

```text
C0: 1 byte      — RTMP version, must be 0x03
C1: 1536 bytes  — 4-byte time, 4-byte zero, 1528 bytes random
S0: 1 byte      — RTMP version, always 0x03
S1: 1536 bytes  — 4-byte time, 4-byte zero, 1528 bytes random (server's own)
S2: 1536 bytes  — echo of C1 (its time field, its zero/echo field, its 1528
                  random bytes, verbatim)
C2: 1536 bytes  — client's echo of S1
```

Wire order:

```text
Client -> Server: C0 + C1
Server -> Client: S0 + S1 + S2
Client -> Server: C2
```

`S0 + S1 + S2` is sent as a single concatenated buffer
(`core::SharedBuffer`) through `IAsyncTransport::async_write`, so the
transport layer's ordered-send machinery can never interleave anything else
ahead of it on the same connection (see `docs/buffer-ownership.md`,
`docs/architecture.md` §7).

`S1`'s time field is the number of milliseconds elapsed since the
`HandshakeSession` was constructed (i.e. since the connection was accepted),
via `core::monotonic_now()` / `core::to_millis` (`core/clock.hpp`). `S1`'s
1528 random bytes come from `core::secure_random_bytes`
(`core/random.hpp`), OpenSSL-backed `RAND_bytes`.

C2's content is bounds-checked (exactly 1536 bytes must arrive) but not
byte-validated against S1 — this mirrors what real RTMP servers/clients do
in practice for the simple handshake and does not weaken security, since C2
carries no information the server trusts for anything beyond "the peer can
complete a TCP round trip".

## State machine

```cpp
enum class HandshakeState : std::uint8_t {
    WaitingForC0,
    WaitingForC1,
    SendingS0S1S2,
    WaitingForC2,
    Completed,
    Failed,
    TimedOut,
};
```

Transitions:

```text
WaitingForC0 --(valid C0)--> WaitingForC1
WaitingForC0 --(invalid version)--> Failed

WaitingForC1 --(1536 bytes of C1 accumulated)--> SendingS0S1S2
SendingS0S1S2 --(send_handler invoked with S0+S1+S2)--> WaitingForC2

WaitingForC2 --(1536 bytes of C2 accumulated)--> Completed

any non-terminal state --(on_timeout())--> TimedOut
any non-terminal state --(malformed/oversized input)--> Failed
```

`Completed`, `Failed`, and `TimedOut` are terminal: further calls to
`on_bytes_received` / `on_timeout` are no-ops.

`HandshakeSession` is pure protocol logic — it never touches a socket or
`liburing` (see `docs/architecture.md` "Architectural Separation"). It is
driven entirely through three injected callbacks:

- `SendHandler` — called with the S0+S1+S2 buffer to transmit.
- `CompleteHandler` — called once, when C2 is fully consumed.
- `FailHandler` — called once, with a `core::Error`, on invalid version,
  oversized/malformed input, or timeout.

## Fragmentation handling

`on_bytes_received` may be called any number of times with arbitrarily
small or large chunks — including a single byte at a time, C0 and C1
arriving in the same read, or C1 and part of C2 arriving in the same read
(the latter is naturally handled because leftover bytes stay in the
internal accumulation buffer and are re-examined on the next call once the
state machine has advanced). Internally it maintains a `std::vector<std::byte>`
accumulator; each call appends incoming bytes, then repeatedly attempts to
consume as much as the current state requires (`try_consume_c0`,
`try_consume_c1`, `try_consume_c2`), falling through to the next state in
the same call if enough bytes are already available. No assumption is ever
made that one `recv()`/one transport receive callback equals one handshake
message.

The wiring point that feeds real, kernel-fragmented TCP data into this is
`IoUringEventLoop::start_handshake` (`src/io/io_uring/event_loop.cpp`),
which installs `session->on_bytes_received` as the `TcpConnection`'s
receive handler. `TcpConnection`/`IoUringEventLoop`'s existing partial-send
queue (from Phase 1) is reused as-is for delivering S0+S1+S2 — the
handshake layer does not reimplement partial-write handling, it just hands
one ordered buffer to `IAsyncTransport::async_write`.

## Timeout behavior

Every accepted connection gets an `IORING_OP_TIMEOUT` armed with purpose
`TimeoutPurpose::Handshake` and duration `config.handshake_timeout`
(default 5000 ms) as soon as the handshake begins, and re-armed after every
partial receive while the handshake is still in progress
(`IoUringEventLoop::arm_handshake_timeout`). If it fires before the
handshake reaches `Completed`, `HandshakeSession::on_timeout()` is invoked,
which transitions the session to `TimedOut` and calls the fail handler; the
fail handler (wired in `start_handshake`) closes the connection through the
same `close_connection` path used for every other error case. If the
handshake completes first, the outstanding timeout operation is cancelled
via `IORING_OP_ASYNC_CANCEL` (`cleanup_handshake_state`), and the
connection reverts to the ordinary idle timeout for whatever comes next.

## Error handling

- **Invalid C0 version** (anything other than `0x03`): the session fails
  immediately with `ErrorCode::MalformedHandshake` — no data has been sent
  back to the peer at that point, and the connection is closed cleanly.
- **Oversized handshake data**: the total bytes fed into a session across
  its whole lifetime cannot exceed `kMaxHandshakeBytes` (`C0 + C1 + C2` =
  3073 bytes exactly). A peer sending more than that before finishing the
  handshake is rejected with `ErrorCode::MalformedHandshake` rather than
  allowed to grow the accumulation buffer without bound — a direct memory
  exhaustion defense.
- **Peer disconnect mid-handshake**: handled at the transport layer — a
  `recv()` returning 0/negative during `handle_receive_completion` closes
  the connection directly (as it did in Phase 1), which also cleans up any
  in-flight `HandshakeSession` and its timeout via
  `IoUringEventLoop::cleanup_handshake_state`.
- **Timeout**: see above.

None of these paths can crash the server — `HandshakeSession` never
indexes past a bounds check, never throws, and every terminal transition
routes through the same connection-close code path.

## Known limitations

- No fake/mock clock exists in this codebase (`core/clock.hpp` wraps
  `std::chrono::steady_clock`/`system_clock` directly with no seam for
  injectable time), so `HandshakeSession`'s own unit tests exercise the
  *timeout code path* (`on_timeout()` called directly) rather than proving
  a real `io_uring` timeout actually elapses after `handshake_timeout`
  milliseconds under test. Adding a fake-clock abstraction purely for this
  would be a new mechanism inconsistent with the rest of the codebase's
  conventions, per the task's explicit guidance — this is called out here
  and in the Phase 2 report as a known gap, not silently skipped.
- Real OBS end-to-end verification was not possible in this environment
  (no OBS instance, no Linux/io_uring-capable host to run the server
  binary on). Substituted with a scripted real-loopback-TCP-socket test
  (`tests/protocol/handshake/handshake_socket_integration_test.cpp`) that
  performs a byte-correct handshake, including fragmentation and
  partial-write patterns, against the actual `HandshakeSession` state
  machine over a real `AF_INET` socket.
