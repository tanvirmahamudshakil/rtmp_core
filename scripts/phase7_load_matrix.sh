#!/usr/bin/env bash
# Phase 7 load scenario matrix (docs/v2_promot.md PHASE 7 "Required
# scenarios"). Runs each scenario against a server, captures both the load
# generator's client-side report and the server's own metrics.
#
# By default it drives apps/rtmp_test_server (the TEST-ONLY poll-based
# front-end, used on hosts where io_uring cannot be built). On a Linux host,
# point SERVER_BIN at the real io_uring server instead:
#
#   SERVER_BIN=./build/release/apps/rtmp_server/rtmp_server \
#   SERVER_ARGS="--config config/server.toml" ./scripts/phase7_load_matrix.sh
#
set -u

BUILD_DIR="${BUILD_DIR:-./build/core-only}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/apps/rtmp_test_server/rtmp_test_server}"
LOADGEN="${LOADGEN:-$BUILD_DIR/apps/rtmp_load_gen/rtmp_load_gen}"
PORT="${PORT:-19360}"
OUT="${OUT:-/tmp/phase7-load}"

mkdir -p "$OUT"
ulimit -n 8192 2>/dev/null || true

run_scenario() {
    local name="$1"; shift
    echo "=============================================================="
    echo "SCENARIO: $name"
    echo "  args: $*"
    echo "=============================================================="

    "$SERVER_BIN" --port "$PORT" --metrics-interval 5 > "$OUT/$name.server.log" 2>&1 &
    local server_pid=$!
    sleep 1

    "$LOADGEN" --port "$PORT" "$@" > "$OUT/$name.client.log" 2>&1
    local client_rc=$?

    sleep 1
    kill -TERM "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    echo "--- client report (exit=$client_rc) ---"
    sed -n '/--- load scenario report ---/,$p' "$OUT/$name.client.log"
    echo "--- server metrics samples ---"
    grep '\[metrics\]' "$OUT/$name.server.log" | tail -3
    echo
    PORT=$((PORT + 1))
}

COMMON="--video-bitrate 2500000 --audio-bitrate 128000 --fps 30 --keyframe-interval 60"

run_scenario "01-pub1-view100"      --publishers 1  --viewers 100 --duration 20 --ramp-up 3000  $COMMON
run_scenario "02-pub1-view500"      --publishers 1  --viewers 500 --duration 20 --ramp-up 5000  $COMMON
run_scenario "03-pub1-view1000"     --publishers 1  --viewers 1000 --duration 20 --ramp-up 8000 $COMMON
run_scenario "04-pub10-view100each" --publishers 10 --viewers 100 --duration 20 --ramp-up 8000  $COMMON
run_scenario "05-viewer-burst"      --publishers 1  --viewers 500 --duration 20 --ramp-up 0     $COMMON
run_scenario "06-slow-viewers"      --publishers 1  --viewers 200 --duration 25 --ramp-up 3000 \
                                    --slow-viewers 0.5 --slow-read-budget 2048 $COMMON
run_scenario "07-publisher-reconnect" --publishers 1 --viewers 100 --duration 25 --ramp-up 3000 \
                                    --publisher-reconnect 5 $COMMON
run_scenario "08-abrupt-disconnect"  --publishers 1 --viewers 200 --duration 20 --ramp-up 3000 \
                                    --abrupt-disconnects 0.5 $COMMON
run_scenario "09-large-keyframes"    --publishers 1 --viewers 100 --duration 20 --ramp-up 3000 \
                                    --video-bitrate 8000000 --audio-bitrate 128000 --fps 30 --keyframe-interval 300

echo "All scenario logs are under $OUT"
