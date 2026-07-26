# Production Gap Analysis (v2 Phase 0)

Each of the 16 suspected issues from `docs/v2_promot.md` section 2 was independently
verified by reading source and, where practical, by grepping for the absence of a wiring
call. File:line references are current as of commit `45edd29`.

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | Executable server may only complete the handshake | **TRUE** | `src/io/io_uring/event_loop.cpp:364-375` — the handshake `complete_handler_` re-arms the idle timeout and clears handshake state but never installs a new receive handler; the accept-time receive handler (`event_loop.cpp:387-389`, forwards to `HandshakeSession::on_bytes_received`) stays installed forever, and that method no-ops once terminal (`src/protocol/handshake/handshake_session.cpp:42`). No other component ever calls `connection->set_receive_handler(...)` a second time (`grep -rn "set_receive_handler" src` -> only the one call site). |
| 2 | ChunkDecoder/ChunkEncoder/CommandSession/media ingest/LiveFanout not connected to the socket lifecycle | **TRUE** | `grep -rl "ChunkDecoder\|ChunkEncoder\|CommandSession\|MediaIngest\|LiveFanout\|StreamRegistry" src/io src/network apps/rtmp_server` returns zero files. `apps/rtmp_server/CMakeLists.txt:1-9` links only `rtmp_server_core` + `rtmp_server_io_uring`, not `rtmp_server_protocol` (where all five live) — the executable cannot call these types even if it wanted to. |
| 3 | Bytes arriving with C2 may be discarded after handshake | **TRUE** | `src/protocol/handshake/handshake_session.cpp:113-124` (`try_consume_c2`): any bytes in `buffer_` beyond the fixed `kHandshakeChunkSize` C2 payload are left in `buffer_`, which is never read again — `complete_handler_()` is invoked with no argument carrying the remainder, and `buffer_` itself is a private member with no accessor. Combined with #2, those bytes are lost permanently. |
| 4 | Publish secret key vs. public playback name not mapped to one internal StreamId | **TRUE** | No `StreamId`/`ApplicationId`/`PublisherId`/`SubscriberId` type exists anywhere (`grep -rn "struct StreamId\|struct ApplicationId\|struct PublisherId\|struct SubscriberId" include src` -> 0 matches). `CommandSession::handle_publish` (`src/protocol/commands/command_session.cpp:244-273`) and `handle_play` (`:275-293`) both take the **same** raw AMF string argument and use it directly as the `StreamRegistry`/`LiveFanout` key (`stream_registry.hpp:35-109`, keyed by `std::string stream_key`). There is no separate "secret key resolves to X, public name resolves to X" indirection — publish and play must literally send the identical string today. |
| 5 | LiveFanout may invoke subscriber callbacks while holding a global mutex | **TRUE** | `src/protocol/commands/live_fanout.cpp:55-69` (`on_video`): `std::lock_guard<std::mutex> lock(mutex_)` is opened, and `dispatch_locked(...)` — which calls `(sub.sink->*method)(message)`, i.e. `PlaybackSink::on_video` — executes inside that same lock scope before the closing brace at line 67. Same pattern in `on_audio` (`:71-84`) and `on_metadata` (`:86-95`). |
| 6 | Media payloads may be deep-copied once per viewer | **TRUE** | `include/rtmp_server/protocol/chunk/chunk_types.hpp:47-53`: `RtmpMessage::payload` is `std::vector<std::byte>` (owning, not `shared_ptr<const Buffer>`). `LiveFanout` stores `RtmpMessage` by value in `gop_cache`/`video_sequence_header`/etc. (`live_fanout.cpp:60-79`) and passes it by const-ref into each subscriber's virtual callback; any subscriber that needs to hold or queue it (`CommandSession::deliver_playback_message`, `command_session.cpp:104-125`) copies the vector. With N viewers, the payload bytes are duplicated N times, not shared. |
| 7 | TCP partial sends may not be handled correctly | **TRUE** | `src/network/tcp_connection.cpp:56-68` (`on_send_completion`): `success = result > 0`; on success the **entire** `write_queue_.front()` buffer is popped unconditionally — there is no tracking of how many bytes of that buffer the `send()` completion actually reported versus the buffer's total size. A completion reporting fewer bytes than requested (a legal partial send) is treated identically to a full send, silently dropping the untransmitted tail of the buffer. |
| 8 | Per-viewer outbound queues may be unbounded | **TRUE** | `src/network/tcp_connection.cpp:16-32` (`async_write`): `write_queue_.push_back(std::move(buffer))` with no check against any byte or packet limit before pushing. `ServerConfig::subscriber_queue_max_bytes`/`subscriber_queue_max_packets` exist (`config.hpp`) but are never read by `TcpConnection` (see config table below) — the field is unconsumed. |
| 9 | Only one io_uring event-loop thread may run despite worker count in config | **TRUE** | `grep -rn "worker_ring_count\|std::thread" src/io/io_uring apps/rtmp_server` matches only the config struct's own field declaration. `apps/rtmp_server/main.cpp` constructs exactly one `IoUringEventLoop` and calls `.run()` once, blocking on the calling thread; nothing reads `config_result.value().worker_ring_count`. |
| 10 | Some configuration options may be parsed but not enforced | **TRUE** | See the full field-by-field table below — 22 of `ServerConfig`'s ~40 fields have zero non-config-parsing usage sites. |
| 11 | Playback tokens may be generated but not validated in play | **TRUE** | `src/protocol/commands/command_session.cpp:275-293` (`handle_play`) takes only a stream-key string and never references `management::Token` or any token/signature check. `grep -rl "management::Token\|Token::validate\|Token::generate"` across `src`/`apps`/`tests` (excluding the management test suite itself) returns nothing — `token.cpp`'s validate logic is implemented and unit-tested in isolation (`tests/management/token_test.cpp`) but has no caller in the RTMP path. |
| 12 | Management library may not be linked to the actual server executable | **TRUE** | `apps/rtmp_server/CMakeLists.txt:3-8`: `target_link_libraries(rtmp_server PRIVATE rtmp_server_core rtmp_server_io_uring rtmp_server_warnings rtmp_server_sanitizers)` — no `rtmp_server_management` (and, as noted in #2, not even `rtmp_server_protocol`). |
| 13 | Load benchmark may use synthetic two-byte payloads, not representative of real capacity | **TRUE** | `apps/load_bench/main.cpp` opens zero sockets (`grep -n "socket\|::send\|::recv\|TcpConnection" apps/load_bench/main.cpp` -> no matches); video frame payloads are literally `{static_cast<std::byte>(keyframe ? 0x17 : 0x27), static_cast<std::byte>(0x01)}` (`main.cpp:126`, 2 bytes). Measures only `LiveFanout::dispatch_locked`'s in-memory map-iteration + virtual-call cost. |
| 14 | Connection shutdown may not correctly remove publishers and subscribers | **PARTIAL** | The logic itself is correct in isolation: `CommandSession::on_connection_closed` (`command_session.cpp:317-333`) unregisters every publishing slot from `StreamRegistry`, calls `LiveFanout::publisher_stopped`, and unsubscribes every playing slot; `tests/protocol/commands/command_session_test.cpp`-style coverage exercises this directly. However, per #2, `CommandSession` is never constructed or driven by the real `IoUringEventLoop`/`TcpConnection` close path (`grep -rn "CommandSession" src/io` -> 0 matches, `TcpConnection::close`/`on_peer_closed` in `src/network/tcp_connection.cpp:45-73` know nothing about `CommandSession`), so in the actually-running server this cleanup code is unreachable dead weight, not a live safety net. |
| 15 | Raw pointers may be used for subscriber/session ownership | **TRUE** | `include/rtmp_server/protocol/commands/live_fanout.hpp:104-107`: `struct Subscriber { PlaybackSink* sink; ... }` — a non-owning raw pointer, and `LiveFanout::subscribe`'s own doc comment (`live_fanout.hpp:88`) states the caller "not owned, caller must outlive the subscription." `CommandSession::handle_play` (`command_session.cpp:289`) derives the subscriber ID via `reinterpret_cast<std::uint64_t>(relay.get())` — a raw pointer address reused as an identity key. |
| 16 | No real HTTP management API, metrics endpoint, or readiness endpoint | **TRUE** | `grep -rli http include src apps` finds no HTTP server implementation (only incidental string matches in comments/paths). `src/observability/metrics.cpp` (35 lines) is an in-memory `counters_`/`gauges_` map with programmatic getters only — no listener, no `/metrics` route, no `/health/live`, `/health/ready`. |

**Summary: 15 of 16 confirmed TRUE, 1 confirmed PARTIAL (correct-but-unreachable), 0 rejected.**

## Configuration enforcement table (task 14/`ServerConfig`)

Built by cross-referencing every field in `include/rtmp_server/core/config.hpp` against
non-`config.cpp`/`config.hpp` usage sites (`grep -rn "\.<field>\b" src apps include`, prior
to any Phase 1+ work).

| Config field | Enforced by | Status |
|---|---|---|
| `rtmp_bind_address`, `rtmp_port` | `IoUringEventLoop::run()` bind/listen (`event_loop.cpp:38-52`) | Consumed |
| `api_bind_address`, `api_port` | — | **Unconsumed** (no HTTP server exists, #16) |
| `public_rtmp_hostname` | — | **Unconsumed** |
| `ring_queue_depth`, `enable_sqpoll`, `sqpoll_idle_ms` | `IoUringContextOptions` passed to `IoUringContext::create` (`main.cpp:62-67`) | Consumed |
| `completion_batch_size` | `io_uring_peek_batch_cqe(&ring, cqes, 64)` uses a hardcoded `64`, not this field (`event_loop.cpp:59,82`) | **Unconsumed** (dead config; value silently ignored even though a batch size is hardcoded) |
| `submission_batch_size` | — | **Unconsumed** |
| `worker_ring_count` | — | **Unconsumed** (#9) |
| `enable_multishot_accept`, `enable_multishot_recv`, `enable_registered_buffers`, `enable_provided_buffer_ring`, `enable_send_zero_copy` | `IoUringCapabilities` probes what the kernel supports but nothing branches on these config flags — `submit_accept`/`submit_receive`/`submit_send` always use the plain (non-multishot) `io_uring_prep_accept`/`recv`/`send` | **Unconsumed** |
| `registered_buffer_count`, `registered_buffer_size` | — | **Unconsumed** (no registered-buffer setup call anywhere) |
| `provided_buffer_count`, `provided_buffer_size` | `BufferPool` constructor (`event_loop.cpp:25`) sizes the plain receive-buffer pool | Consumed (as a plain pool, not an actual io_uring provided-buffer ring) |
| `maximum_connections` | `handle_accept_completion` rejects over-limit accepts (`event_loop.cpp:329-333`) | Consumed |
| `maximum_connections_per_ip` | — | **Unconsumed** |
| `maximum_publishers` | — | **Unconsumed** (`StreamRegistry::register_publisher` enforces only "one publisher per key", not a global publisher count) |
| `maximum_viewers_per_stream` | — | **Unconsumed** |
| `input_chunk_size`, `output_chunk_size` | Read by `ChunkDecoder`/`ChunkEncoder` constructors in their own unit tests | Consumed **only inside `rtmp_server_protocol`**, which is never instantiated by the real server (#2) — effectively unconsumed in production |
| `maximum_rtmp_message_size` | Referenced in `ChunkDecoder` (protocol layer) | Same caveat as above |
| `handshake_timeout` | `arm_handshake_timeout` (`event_loop.cpp:395-433`) | Consumed |
| `authentication_timeout` | — | **Unconsumed** |
| `idle_timeout` | `arm_idle_timeout` (`event_loop.cpp:194-228`) | Consumed |
| `write_timeout` | — | **Unconsumed** (no write-timeout SQE is ever armed) |
| `publisher_inactivity_timeout` | — | **Unconsumed** |
| `gop_cache_max_duration`, `gop_cache_max_bytes`, `gop_cache_max_packets` | — | **Unconsumed** (`LiveFanout`'s `gop_cache` is an unbounded `std::deque`, only cleared on next keyframe) |
| `subscriber_queue_max_bytes`, `subscriber_queue_max_packets` | `CommandSession::deliver_playback_message` checks `max_queued_playback_bytes_`, a **constructor default**, not this config field (`command_session.cpp:108`) | **Unconsumed** from config (a hardcoded default is used instead) |
| `recording_directory`, `recording_enabled` | `Recorder`/wiring code | Consumed |
| `recording_max_size` | — | **Unconsumed** |
| `recording_queue_max_bytes` | — | **Unconsumed** (`Recorder` has its own bounded-queue constructor parameter, not sourced from this field) |
| `database_type`, `database_connection` | — | **Unconsumed** (`SqliteStore` takes an explicit path in its own tests/constructor, never read from `ServerConfig`) |
| `token_signing_secret` | Referenced in management test setup (`tests/management/token_test.cpp`) | Consumed **only in tests**, never read by the real server (#11) |
| `api_authentication_secret` | — | **Unconsumed** |
| `log_level` | `main.cpp:57-60` sets `Logger` level | Consumed |
| `metrics_enabled` | — | **Unconsumed** (no code branches on it; `Metrics` always increments) |

22 of 40 fields have no production consumer at all; a further handful (chunk sizes, token
secret) are only exercised inside unit tests of libraries the real executable doesn't link.

## Compiler warnings / build-system findings (task 13)

* Clean build, zero errors, on AppleClang 21 / macOS (Darwin, arm64) with
  `RTMP_SERVER_CORE_ONLY` implicitly forced by the `CMAKE_SYSTEM_NAME STREQUAL "Linux"` guard
  (see `CMakeLists.txt:60`) — the `io_uring`/`rtmp_server` targets are skipped, not built and
  failing; this is documented, expected behavior on this platform, not a defect.
* Warnings observed during the build (`cmake --build build-debug -j`):
  * `tests/unit/core/config_test.cpp:15` — `std::tmpnam` deprecated (should use `mkstemp`).
  * `src/media/flv/flv_writer.cpp:50` — unused function `append_amf0_number`.
  * `tests/media/flv_writer_test.cpp:60` — signed/unsigned conversion warning.
  * One `char8_t`->`char32_t` warning inside vendored GoogleTest (`_deps`, not our code).
  * Linker warnings "ignoring duplicate libraries: librtmp_server_protocol.a" for the three
    fuzz targets (harmless, comes from `fuzz/CMakeLists.txt` listing the library twice).
* No missing source files; no linker errors on this platform for anything actually configured
  to build here.
* Unlinked management/protocol modules: see #2/#12 above — this is the single most
  significant build-system finding, since it means the "server" target is architecturally
  incapable of running RTMP today, independent of any runtime logic bugs.
