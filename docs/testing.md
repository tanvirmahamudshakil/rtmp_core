# Testing

## Scope

How this codebase is tested: unit/integration tests (every phase),
sanitizers (every phase), fuzzing and load testing (Phase 9). See each
`docs/phase*-checklist.md` for the specific test list a given phase added.

## Unit and integration tests

GoogleTest, one executable per module, discovered via `gtest_discover_tests`:

| Executable | What it covers |
|---|---|
| `rtmp_server_unit_tests` | `core/` (config, random, hmac, result, byte reader/writer) and `observability/` (audit log, metrics) |
| `rtmp_server_protocol_tests` | handshake, chunk codec, AMF0 codec, `CommandSession` (connect/publish/play/media routing/fan-out), media ingest |
| `rtmp_server_media_tests` | FLV byte format |
| `rtmp_server_recording_tests` | `recording::Recorder` orchestration |
| `rtmp_server_management_tests` | `StreamManager`, tokens, URL builder, authorization cache, persistence integration |
| `rtmp_server_persistence_tests` | `SqliteStore` |

```
$ cmake --preset core-only
$ cmake --build --preset core-only
$ ctest --preset core-only
```

(`core-only` is the macOS-buildable preset — no io_uring/Linux transport;
see every phase's "Build commands". On Linux, the `debug` preset builds and
tests the real transport target too.)

## Sanitizers

`RTMP_SERVER_ENABLE_ASAN`/`RTMP_SERVER_ENABLE_TSAN` (mutually exclusive;
`cmake/Sanitizers.cmake`) compile every target (including tests) with
`-fsanitize=address,undefined` or `-fsanitize=thread`. Every phase's
checklist runs the full test suite under `asan` and reports 100% pass with
zero sanitizer findings before being considered done — this has held for
Phases 0 through 9 (190 tests as of Phase 9).

```
$ cmake --preset asan
$ cmake --build --preset asan
$ ctest --preset asan
```

## Fuzzing

`fuzz/` (Phase 9) has three libFuzzer-shaped harnesses for the parsers that
consume untrusted bytes (network or file):

- `fuzz_amf0_decoder` — `protocol::amf0::decode_all`
- `fuzz_chunk_decoder` — `protocol::chunk::ChunkDecoder::on_bytes_received`
  (fed in two fragments, to exercise cross-call reassembly state)
- `fuzz_flv_parser` — `media::flv::parse_flv`

Every harness builds two ways from the same source file:

1. **Always** (no extra flags): as a plain executable whose `main()` reads
   files from argv and replays them through `LLVMFuzzerTestOneInput` — a
   corpus-replay/regression tool that needs no fuzzing engine.
   ```
   $ ./build/core-only/fuzz/fuzz_amf0_decoder crash-input-1 crash-input-2 ...
   ```
2. **With `-DRTMP_SERVER_ENABLE_FUZZING=ON`** (Clang only): compiled with
   `-fsanitize=fuzzer,address` and linked against libFuzzer's own `main()`,
   for real coverage-guided fuzzing:
   ```
   $ cmake --preset core-only -DRTMP_SERVER_ENABLE_FUZZING=ON
   $ cmake --build --preset core-only --target fuzz_amf0_decoder
   $ ./build/core-only/fuzz/fuzz_amf0_decoder -max_total_time=60
   ```

**Status on this project's development host (macOS, Apple Clang from
Xcode)**: mode 1 (corpus replay) was built and smoke-tested successfully.
Mode 2 was attempted and **fails to link**: Apple's Clang does not ship
`libclang_rt.fuzzer_osx.a` (libFuzzer's runtime is an LLVM.org Clang/
compiler-rt feature, not part of Xcode's toolchain). Actual fuzzing —
accumulating corpus, coverage, finding crashes — has not been run as part of
this phase; it should work as documented on a Linux host with a full LLVM
toolchain (`apt-get install clang libfuzzer` or similar), or on macOS with
Homebrew's `llvm` package's Clang instead of Xcode's.

## Load testing

`apps/load_bench` (Phase 9) is an in-process synthetic load generator for
the RTMP protocol/fan-out layer: it drives real `CommandSession`/
`StreamRegistry`/`LiveFanout` objects (the same classes production traffic
would go through) with in-memory `RtmpMessage` handoffs instead of socket
I/O, and reports fanned-out messages/sec.

```
$ ./build/core-only/apps/load_bench/load_bench --streams 4 --viewers-per-stream 50 --frames 1000
load_bench: streams=4 viewers_per_stream=50 frames=1000
delivered 200000 viewer messages in 0.083s (2402446 messages/sec)
```

This measures the protocol/fan-out layer's own ceiling, not network I/O or
disk I/O — those depend on the (not-yet-built-on-this-host) io_uring
transport and are not something a socket-free load generator can exercise
meaningfully. A true end-to-end load test (real TCP connections, real RTMP
clients) needs the Linux transport target and is out of scope for what's
verifiable in this development environment — see
`docs/phase9-checklist.md` "Known limitations".

## Static analysis

`.clang-tidy` + `cmake/StaticAnalysis.cmake` (`-DRTMP_SERVER_ENABLE_CLANG_TIDY=ON`),
present since Phase 0. Not run in this environment (`clang-tidy` not
installed) — see `docs/security.md` "Static analysis".
