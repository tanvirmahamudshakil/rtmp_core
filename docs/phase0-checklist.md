# Phase 0 Implementation Checklist

- [x] Inspect project folder (empty: only `docs/rtmp_promot.md`, empty `sample.tsxt`)
- [x] Print file tree
- [x] Inspect build files — none exist
- [x] Inspect existing source — none exists
- [x] Identify conflicts — none, greenfield init
- [x] Define module boundaries — see `docs/architecture.md` §4
- [x] Define connection ownership — see `docs/architecture.md` §5
- [x] Define operation ownership — see `docs/architecture.md` §6
- [x] Define buffer ownership — see `docs/architecture.md` §7
- [x] Define threading model — see `docs/architecture.md` §8
- [x] Define shutdown model — see `docs/architecture.md` §9
- [x] Define timestamp model — see `docs/architecture.md` §10
- [x] Define security boundaries — see `docs/architecture.md` §11
- [x] Create implementation checklist (this file)

## Repository Scaffolding (Phase 0 close-out)

- [x] Full directory tree created per spec's "Required Repository Structure"
- [x] `.gitignore`, `LICENSE` (MIT), `.clang-format`, `.clang-tidy`
- [x] `README.md` with all required sections (most marked TODO pending later phases)
- [x] All 18 `docs/*.md` files present — `architecture.md` filled in, remaining 17 are stubs
  filled in as their phase lands
- [x] `config/server.example.yaml`, `config/logging.example.yaml`, `config/environment.example`
- [x] `scripts/*.sh` placeholders (fail loudly with "not yet implemented" until Phase 1)
- [ ] `CMakeLists.txt`, `CMakePresets.json`, `cmake/*.cmake` — intentionally deferred to Phase 1
  (Phase 0 is design/scaffolding only, not build-system code)
- [ ] Any C++ source under `include/`, `src/`, `apps/`, `tests/` — deferred to Phase 1

Next: Phase 1 — Core and io_uring TCP Foundation (CMake, presets, warnings, sanitizer builds,
logger, config, fd RAII, buffers, capability detection, io_uring context, event loop,
async accept/receive/send, timeouts, cancellation, connection registry, graceful shutdown, tests).
