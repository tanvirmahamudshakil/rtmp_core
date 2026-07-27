#!/usr/bin/env bash
# Phase 8 release gate (docs/v2_promot.md PHASE 8 "Release gates").
#
# The spec requires a release to FAIL when any of the following holds:
#
#   1. Tests fail
#   2. Sanitizers report errors
#   3. Required configuration is missing
#   4. Database migrations fail
#   5. Unsupported insecure defaults are used
#   6. Compiler warnings exceed the agreed policy
#
# This script is the executable form of that list. It exits non-zero on the
# first violation, so it can be the single command a CI pipeline or a release
# runbook invokes before an artefact is allowed to be published.
#
# Gates 3 and 5 are enforced in two places on purpose: ServerConfig::validate()
# fails the server at startup (so a bad config can never reach a listening
# state, whatever CI did), and gate 3/5 below proves that enforcement still
# works before shipping the binary that relies on it.
#
# Usage:
#   scripts/release_gate.sh                 # full gate
#   PRESET=core-only scripts/release_gate.sh
#
# Environment:
#   PRESET          base build preset (default: auto-detected per platform)
#   SKIP_TSAN=1     skip the ThreadSanitizer stage (it is the slowest)
#   FUZZ_RUNS       executions per fuzz target (default 200000; 0 disables)

set -uo pipefail
cd "$(dirname "$0")/.."

# --- Platform detection ----------------------------------------------------
# The RTMP transport is io_uring and therefore Linux-only. On any other host
# only the platform-independent subset can be built at all, so the gate runs
# against the core-only presets and says so loudly rather than silently
# testing less than it claims to.
UNAME="$(uname -s)"
if [[ -n "${PRESET:-}" ]]; then
    BASE_PRESET="$PRESET"
elif [[ "$UNAME" == "Linux" ]]; then
    BASE_PRESET="debug"
else
    BASE_PRESET="core-only"
fi

if [[ "$BASE_PRESET" == "core-only" ]]; then
    ASAN_PRESET="asan-core-only"
    TSAN_PRESET="tsan-core-only"
else
    ASAN_PRESET="asan"
    TSAN_PRESET="tsan"
fi

FUZZ_RUNS="${FUZZ_RUNS:-200000}"

FAILURES=()
STAGE=""

log()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
pass() { printf '   \033[32mPASS\033[0m  %s\n' "$*"; }
fail() { printf '   \033[31mFAIL\033[0m  %s\n' "$*"; FAILURES+=("$STAGE: $*"); }

log "Release gate configuration"
printf '   host           : %s\n'  "$UNAME"
printf '   base preset    : %s\n'  "$BASE_PRESET"
printf '   sanitizers     : %s, %s\n' "$ASAN_PRESET" "$TSAN_PRESET"
printf '   fuzz runs/target: %s\n' "$FUZZ_RUNS"
if [[ "$UNAME" != "Linux" ]]; then
    printf '\n   \033[33mNOTE\033[0m: not a Linux host. The io_uring transport\n'
    printf '         (src/io/io_uring, apps/rtmp_server) cannot be compiled or\n'
    printf '         tested here. This gate covers the platform-independent\n'
    printf '         subset only and is NOT sufficient to approve a release.\n'
fi

# ---------------------------------------------------------------------------
# Gate 6: compiler warnings exceed policy
# ---------------------------------------------------------------------------
# Policy: zero warnings from first-party code. The build already enables
# -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion -Wformat=2
# -Wundef -Wnull-dereference -Wdouble-promotion (cmake/CompilerWarnings.cmake);
# this gate asserts none of them fire. Warnings originating in _deps/ are
# third-party (GoogleTest) and are excluded -- we do not control that code.
log "Gate 6: build with zero first-party compiler warnings"
STAGE="warnings"
BUILD_LOG="$(mktemp)"
cmake --preset "$BASE_PRESET" >/dev/null 2>&1
if ! cmake --build --preset "$BASE_PRESET" 2>&1 | tee "$BUILD_LOG" | grep -E '(error|warning):' ; then
    :  # no matches from grep is the good case
fi

if grep -qE '^[^ ]*: error:|error:' "$BUILD_LOG"; then
    fail "build produced errors"
else
    pass "build succeeded"
fi

WARNINGS="$(grep -E 'warning:' "$BUILD_LOG" | grep -v '/_deps/' | grep -v 'ld: warning: ignoring duplicate' || true)"
WARN_COUNT="$(printf '%s' "$WARNINGS" | grep -c . || true)"
if [[ "$WARN_COUNT" -gt 0 ]]; then
    fail "$WARN_COUNT first-party compiler warning(s)"
    printf '%s\n' "$WARNINGS" | sed 's/^/        /'
else
    pass "zero first-party compiler warnings"
fi

# ---------------------------------------------------------------------------
# Gate 1: tests fail
# ---------------------------------------------------------------------------
log "Gate 1: full test suite"
STAGE="tests"
if ctest --preset "$BASE_PRESET" --output-on-failure; then
    pass "all tests passed"
else
    fail "test suite failed"
fi

# ---------------------------------------------------------------------------
# Gates 3 and 5: required configuration missing / insecure defaults used
# ---------------------------------------------------------------------------
# Asserted through the config validation unit tests, which are the executable
# specification of both rules (short secrets, placeholder secrets, reused
# secrets, zero-entropy secrets, unbounded queues, missing required paths).
log "Gates 3 + 5: configuration validation and insecure-default rejection"
STAGE="config"
if ctest --preset "$BASE_PRESET" -R 'ConfigTest|ServerConfigValidate' --output-on-failure; then
    pass "configuration gates enforced"
else
    fail "configuration validation gates are not enforced"
fi

# ---------------------------------------------------------------------------
# Gate 4: database migrations fail
# ---------------------------------------------------------------------------
log "Gate 4: persistence schema / migrations"
STAGE="migrations"
if ctest --preset "$BASE_PRESET" -R 'Sqlite|Persistence|persistence' --output-on-failure; then
    pass "persistence schema applies cleanly"
else
    fail "database migration/schema tests failed"
fi

# ---------------------------------------------------------------------------
# Gate 2: sanitizers report errors
# ---------------------------------------------------------------------------
log "Gate 2a: AddressSanitizer + UndefinedBehaviorSanitizer"
STAGE="asan"
cmake --preset "$ASAN_PRESET" >/dev/null 2>&1
if cmake --build --preset "$ASAN_PRESET" >/dev/null 2>&1 && ctest --preset "$ASAN_PRESET" --output-on-failure; then
    pass "ASan + UBSan clean"
else
    fail "ASan/UBSan reported errors"
fi

if [[ "${SKIP_TSAN:-0}" != "1" ]]; then
    log "Gate 2b: ThreadSanitizer"
    STAGE="tsan"
    cmake --preset "$TSAN_PRESET" >/dev/null 2>&1
    if cmake --build --preset "$TSAN_PRESET" >/dev/null 2>&1 && ctest --preset "$TSAN_PRESET" --output-on-failure; then
        pass "TSan clean"
    else
        fail "TSan reported data races"
    fi
else
    printf '\n   SKIP  ThreadSanitizer (SKIP_TSAN=1)\n'
fi

# ---------------------------------------------------------------------------
# Gate 2c: fuzz targets (a crash here is a sanitizer error by another route)
# ---------------------------------------------------------------------------
if [[ "$FUZZ_RUNS" != "0" ]]; then
    log "Gate 2c: fuzz targets under ASan"
    STAGE="fuzz"
    FUZZ_DIR="./build/${ASAN_PRESET}/fuzz"
    for target in fuzz_amf0_decoder fuzz_chunk_decoder fuzz_flv_parser fuzz_handshake fuzz_token_parser; do
        if [[ ! -x "$FUZZ_DIR/$target" ]]; then
            fail "$target was not built"
            continue
        fi
        if "$FUZZ_DIR/$target" --runs "$FUZZ_RUNS" --seed "$(date +%s)" --max-len 8192 >/dev/null 2>&1; then
            pass "$target: $FUZZ_RUNS executions, no crashes"
        else
            fail "$target crashed -- rerun without redirection to capture the report"
        fi
    done
fi

# ---------------------------------------------------------------------------
log "Result"
if [[ ${#FAILURES[@]} -eq 0 ]]; then
    printf '   \033[32mRELEASE GATE PASSED\033[0m\n'
    if [[ "$UNAME" != "Linux" ]]; then
        printf '   (platform-independent subset only -- see note above)\n'
    fi
    exit 0
fi

printf '   \033[31mRELEASE GATE FAILED\033[0m -- %d violation(s):\n' "${#FAILURES[@]}"
for f in "${FAILURES[@]}"; do printf '     - %s\n' "$f"; done
exit 1
