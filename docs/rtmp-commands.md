# RTMP Commands

> Status: implemented in Phase 4. See `docs/phase4-checklist.md` for the
> phase tracking record.

## Scope

The RTMP command layer: `connect`, `releaseStream`, `FCPublish`,
`createStream`, `publish`, `play`, and `deleteStream`, built directly on
top of AMF0 (`docs/amf0.md`) and the chunk engine (`docs/chunk-parser.md`).
Implemented by:

* `rtmp_server::protocol::commands::CommandSession`
  (`include/rtmp_server/protocol/commands/command_session.hpp`,
  `src/protocol/commands/command_session.cpp`) — one instance per
  connection. Pure protocol logic: no sockets, no io_uring, consumes
  already-reassembled `chunk::RtmpMessage`s and produces `chunk::
  RtmpMessage`s in return via a handler callback, exactly like
  `protocol::handshake::HandshakeSession` is driven by bytes and produces
  bytes via a handler (docs/architecture.md "Architectural Separation").
* `rtmp_server::protocol::commands::StreamRegistry`
  (`include/rtmp_server/protocol/commands/stream_registry.hpp`, header-only)
  — process-wide map from stream key to the connection currently publishing
  it. Thread-safe (internally mutex-guarded), transport-independent
  (keyed by an opaque `connection_id`, not a `network::TcpConnection`).

## Message flow

A typical OBS (or FMLE-style) publish session, in order:

```text
C->S  connect(app, tcUrl, ...)             S->C  _result  (NetConnection.Connect.Success)
C->S  releaseStream(streamKey)             S->C  _result
C->S  FCPublish(streamKey)                 S->C  _result
C->S  createStream()                       S->C  _result  (numeric stream ID)
C->S  publish(streamKey, "live")           S->C  onStatus (NetStream.Publish.Start | Bad.Name)
...                                        (audio/video messages — Phase 5)
C->S  deleteStream(streamId)               (no reply)
```

A playback session is simpler: `connect` -> `createStream` -> `play(name)`
-> `onStatus (NetStream.Play.Start)`.

`CommandSession::handle_message()` is the single entry point: it inspects
`RtmpMessage::message_type_id`, and for `MessageTypeId::Amf0Command` (20)
decodes the payload with `amf0::decode_all()` into `(name, transaction_id,
command_object, arguments...)`, then dispatches by `name`. All other
message types (including `Amf0Data`/18, used for `@setDataFrame` metadata)
are ignored by this phase — media/metadata handling belongs to Phase 5
(Media Ingest).

A malformed command payload (fails to decode, or decodes to a leading
value that isn't a String) is silently dropped: there is no trustworthy
transaction ID to reply on, so — matching `ChunkDecoder`'s policy of
rejecting rather than guessing at recovery for malformed input — the
message is discarded and the connection is left connected (the *chunk*
layer would already have failed the connection for framing-level
corruption; a syntactically valid but semantically empty/wrong AMF0
payload is different and not considered fatal).

## Command handlers

* **connect** — extracts `app` from the command object, stores it, marks
  the session connected, and replies `_result` with a `(fmsVer,
  capabilities)` properties object and a `(level="status",
  code="NetConnection.Connect.Success", description, objectEncoding=0)`
  information object.
* **releaseStream** / **FCPublish** — FMLE/OBS-classic pre-publish
  handshake commands. Both simply reply `_result(transactionId, null,
  undefined)` — there is nothing stateful to validate yet (the actual
  stream key is checked at `publish` time), matching how most RTMP servers
  treat these as no-ops beyond acknowledging the transaction.
* **createStream** — allocates the next message-stream ID (a simple
  monotonically increasing counter starting at 1, per connection) and
  replies `_result(transactionId, null, streamId)`.
* **publish** — reads the stream key (first argument) and looks it up via
  the injected `StreamKeyValidator` (`std::function<bool(app, key)>`,
  supplied by the session owner — a real deployment plugs in signed-URL or
  database validation here; tests inject always-true/always-false/specific
  -key lambdas). If authorized, registers `(app, key, connection_id,
  message_stream_id)` in the `StreamRegistry` (which itself rejects a
  second, different connection publishing the same key), transitions the
  stream slot to `NetStreamState::Publishing`, and replies `onStatus`
  with `NetStream.Publish.Start`. If unauthorized, already taken by
  another connection, or the key is empty, replies `onStatus` with level
  `"error"` and code `NetStream.Publish.BadName` instead, and the stream
  slot is left in `NetStreamState::Idle` (or whatever it was).
* **play** — reads the stream name (first argument), transitions the
  stream slot to `NetStreamState::Playing`, and replies `onStatus` with
  `NetStream.Play.Start`. (Actually serving media to a playing stream is
  Phase 6 — Playback; this phase only implements the command handshake.)
* **deleteStream** — reads the numeric stream ID argument, and if that
  slot was publishing, unregisters it from the `StreamRegistry` and clears
  the slot. No reply is sent (matches common server behavior — there is no
  meaningful `_result`/`onStatus` shape clients expect for this command).

`CommandSession::on_connection_closed()` is the analogous cleanup for an
abrupt disconnect rather than an explicit `deleteStream`: it walks every
stream slot this session owns, unregisters any that were publishing, and
clears its own state. The session owner (a later phase's connection
lifecycle code — not wired up here, see "Known limitations") is expected
to call this from its close path.

## Response shapes

All responses are single AMF0-command RTMP messages
(`MessageTypeId::Amf0Command`, sent on chunk stream ID 3 by convention —
csid 2 is reserved for protocol control):

* `_result(transactionId, properties, information)` — success reply to a
  NetConnection-level command (`connect`) or transaction (`releaseStream`,
  `FCPublish`, `createStream`).
* `_error(transactionId, null, information)` — provided
  (`CommandSession::send_error`) for completeness/future use by handlers
  that need to reject a transaction-based command outright; none of the
  currently-implemented handlers need it since `publish`'s rejection path
  uses `onStatus` (matching what OBS and other encoders actually key off
  of for publish failures, rather than `_error`).
* `onStatus(0, null, information)` — NetStream-level status change
  (`publish`, `play`). Sent on the *stream's* message stream ID (not 0),
  since `onStatus` is scoped to a particular NetStream.

`information` is always an AMF0 Object with at least `level` (`"status"`
or `"error"`), `code` (e.g. `"NetStream.Publish.Start"`), and
`description`.

## Stream key validation and the registry

Authorization is deliberately not hardcoded: `StreamKeyValidator` is a
`std::function<bool(std::string_view app, std::string_view stream_key)>`
supplied to `CommandSession`'s constructor, so a real deployment can plug
in whatever policy it needs (signed URL verification, a database lookup,
a static allow-list) without touching `CommandSession` itself, and tests
can inject trivial always-true/always-false/specific-key lambdas.

`StreamRegistry` (`stream_registry.hpp`) is intentionally minimal — it
answers "is this stream key currently published, and by whom" and nothing
more (no bitrate stats, no GOP cache, no subscriber list). It is
transport-independent by design (keyed by `connection_id`, a plain
`std::uint64_t`, not a `network::TcpConnection`), which is both why it's
possible to unit test without sockets and why it lives under
`protocol/commands/` rather than `server/`: everything under `src/protocol`
builds under `RTMP_SERVER_CORE_ONLY` (no io_uring/liburing dependency),
and `server/connection/connection_registry.hpp` (the existing registry in
`include/rtmp_server/server/`) already depends on
`network::TcpConnection`, which does not build on this host. Later phases
(Media Ingest, Playback) are expected to extend `StreamRegistry` with
whatever additional per-stream state they need (GOP cache handle,
subscriber list, bitrate counters, ...), not replace it.

## Known limitations

* Not wired into `event_loop.cpp`/`IoUringEventLoop` or any per-connection
  session object — same deferral Phase 2 (handshake) and Phase 3 (chunk
  engine) both used, since `io_uring` cannot be built or tested on this
  host, and there is no connection-lifecycle-to-command-dispatch wiring
  yet for `CommandSession` to plug into. `CommandSession` is fully built,
  tested, and sanitizer-clean as a standalone protocol-layer component.
* AMF3 (message type 17) commands are not handled — see
  `docs/amf0.md` "Known limitations". A client that only speaks AMF3
  commands is not supported.
* `play`'s `start`/`duration`/`reset` optional arguments (per the RTMP
  spec's full `play` signature) are accepted (ignored) rather than acted
  on — actually serving a stream (seeking, live-vs-VOD semantics) is
  Playback (a later phase), not this one.
* `NetStream.Publish.BadName` is used for every publish-rejection reason
  (unauthorized key, empty key, key already published by another
  connection) rather than a more specific code per case — this matches
  what OBS and most encoders actually branch on (the code prefix
  `NetStream.Publish.Bad*` vs `NetStream.Publish.Start`), and RTMP has no
  more specific standard code for "someone else already owns this key".
* No AMF3/typed-object/XML command arguments are supported (see
  `docs/amf0.md`) — connect/createStream/publish/play/deleteStream from
  every mainstream RTMP client only ever use Number/Boolean/String/
  Object/Null/Undefined, which is exactly what's implemented.
