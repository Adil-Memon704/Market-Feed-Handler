#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build"
BUILD_TYPE="${1:-Release}"

echo "================================================================"
echo " Market Data Feed Handler — Build"
echo " Build type: $BUILD_TYPE"
echo "================================================================"

# Detect CPU cores
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS=ON \
    -DBUILD_BENCHMARKS=ON

cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "Build complete. Binaries:"
echo "  $BUILD_DIR/exchange_server"
echo "  $BUILD_DIR/feed_client"
if [ -f "$BUILD_DIR/run_tests" ]; then
    echo "  $BUILD_DIR/run_tests"
fi
