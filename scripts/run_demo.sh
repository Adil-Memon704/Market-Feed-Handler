#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"

SERVER="$BUILD_DIR/exchange_server"
CLIENT="$BUILD_DIR/feed_client"

if [ ! -f "$SERVER" ] || [ ! -f "$CLIENT" ]; then
    echo "Binaries not found. Run scripts/build.sh first."
    exit 1
fi

# Config
PORT=9876
SYMBOLS=100
RATE=100000

echo "================================================================"
echo " NSE Market Data Feed Handler — Demo"
echo " Server: port=$PORT, symbols=$SYMBOLS, rate=$RATE msg/s"
echo "================================================================"
echo ""

# Start server in background
echo "[demo] Starting exchange server..."
"$SERVER" -p "$PORT" -s "$SYMBOLS" -r "$RATE" &
SERVER_PID=$!

# Give server a moment to bind
sleep 0.5

# Cleanup on exit
cleanup() {
    echo ""
    echo "[demo] Shutting down..."
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    # Restore terminal
    stty sane 2>/dev/null || true
    echo "[demo] Done."
}
trap cleanup EXIT INT TERM

echo "[demo] Starting feed client..."
"$CLIENT" -h 127.0.0.1 -p "$PORT" -s "$SYMBOLS" -e /tmp/latency.csv

echo "[demo] Latency histogram saved to /tmp/latency.csv"
