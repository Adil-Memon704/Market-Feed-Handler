#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"

echo "================================================================"
echo " Market Data Feed Handler — Microbenchmarks"
echo "================================================================"
echo ""

run_bench() {
    local name="$1"
    local bin="$BUILD_DIR/$2"
    if [ ! -f "$bin" ]; then
        echo "SKIP: $bin not found (run build.sh first)"
        return
    fi
    echo "── $name ────────────────────────────────────"
    "$bin"
    echo ""
}

run_bench "Parser Throughput"     "bench_parser"
run_bench "SymbolCache Latency"   "bench_cache"
run_bench "LatencyTracker Record" "bench_latency"

echo "All benchmarks complete."
