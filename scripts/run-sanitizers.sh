#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
