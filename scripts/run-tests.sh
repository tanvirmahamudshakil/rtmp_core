#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
