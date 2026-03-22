# Market Data Feed Handler
### Low-Latency Multi-Asset Market Data Processor for NSE Co-location

---

## Quick Start

```bash
# 1. Build
./scripts/build.sh Release

# 2. Run full demo (server + client in one command)
./scripts/run_demo.sh

# 3. Or run server and client separately:
./build/exchange_server -p 9876 -s 100 -r 100000
./build/feed_client     -h 127.0.0.1 -p 9876 -s 100
```

Press **`q`** in the client to quit. Press **`r`** to reset latency statistics.

---

## Architecture

```
Exchange Simulator (Server)          Feed Handler (Client)
──────────────────────────           ────────────────────────────────
tick_thread   → GBM + RNG            network_thread → epoll (ET) + recv()
               ↓ SPSC queue                           ↓ ByteRingBuffer
broadcast_thread → send() all        parse_thread   → Parser + SymbolCache
accept_thread → epoll (LT)           visualize_thread → ANSI terminal (500ms)
                                     reconnect_thread → backoff + retry
```

---

## Components

| Component | File | Description |
|-----------|------|-------------|
| Protocol | `include/protocol.h` | Wire format, checksums, timestamps |
| Ring Buffer | `include/common/ring_buffer.h` | SPSC byte stream + object queue |
| Symbol Cache | `include/common/cache.h` | Seqlock-protected market state |
| Latency Tracker | `include/common/latency_tracker.h` | Histogram-based percentiles |
| Memory Pool | `include/common/memory_pool.h` | Slab allocator, zero heap in hot path |
| Tick Generator | `include/server/tick_generator.h` | GBM + Box-Muller + Xoshiro256++ |
| Client Manager | `include/server/client_manager.h` | Per-client send buffers, slow-client drop |
| Exchange Simulator | `include/server/exchange_simulator.h` | epoll server, 3 threads |
| Socket | `include/client/socket.h` | Non-blocking TCP + reconnect policy |
| Parser | `include/client/parser.h` | Zero-copy binary parser |
| Visualizer | `include/client/visualizer.h` | ANSI terminal, raw codes |
| Feed Handler | `include/client/feed_handler.h` | Client orchestrator, 4 threads |

---

## Wire Protocol

```
Header (16 bytes):
  uint16_t  msg_type      0x01=Trade, 0x02=Quote, 0x03=Heartbeat
  uint32_t  seq_num       Monotonically increasing
  uint64_t  timestamp_ns  Nanoseconds since epoch (CLOCK_REALTIME)
  uint16_t  symbol_id     0-499

Trade payload (12 bytes):   price(f64) + quantity(u32)
Quote payload (24 bytes):   bid_price(f64) + bid_qty(u32) + ask_price(f64) + ask_qty(u32)
Checksum (4 bytes):         XOR of all preceding bytes (4-byte words)
```

---

## Server Options

```
./build/exchange_server [options]
  -p PORT       Listen port (default: 9876)
  -s SYMBOLS    Number of symbols (default: 100, max: 500)
  -r RATE       Tick rate messages/sec (default: 100000)
  -f            Enable fault injection (sequence gaps)
```

## Client Options

```
./build/feed_client [options]
  -h HOST       Server host (default: 127.0.0.1)
  -p PORT       Server port (default: 9876)
  -s SYMBOLS    Number of symbols in cache (default: 100)
  -S SYM_IDS    Comma-separated symbol IDs to subscribe (e.g. 0,1,2)
  -e FILE       Export latency histogram to CSV on exit
```

---

## Running Tests

```bash
# Requires: sudo apt install libgtest-dev
./scripts/build.sh Debug
cd build && ctest --output-on-failure
```

## Running Benchmarks

```bash
./scripts/build.sh Release
./scripts/benchmark_latency.sh
```

---

## Performance Targets

| Metric | Target | Component |
|--------|--------|-----------|
| Parser throughput | > 500K msg/s | `bench_parser` |
| Cache read latency (mean) | < 50 ns | `bench_cache` |
| Cache read latency (p999) | < 100 ns | `bench_cache` |
| Latency record overhead | < 30 ns | `bench_latency` |
| End-to-end T0→T4 (p99) | < 50 µs | Loopback |

---

## Dependencies

| Library | Purpose | Install |
|---------|---------|---------|
| Linux kernel ≥ 5.10 | epoll, eventfd, timerfd | — |
| GCC ≥ 11 or Clang ≥ 13 | C++17, `alignas`, atomics | `sudo apt install build-essential` |
| CMake ≥ 3.16 | Build system | `sudo apt install cmake` |
| GTest (optional) | Unit tests | `sudo apt install libgtest-dev` |

No other external dependencies. All data structures are implemented from scratch.

---

## Design Highlights

- **Zero heap allocation in hot path** — all buffers pre-allocated at startup
- **Seqlock** on SymbolCache — single-writer, O(1) non-blocking reads
- **Edge-triggered epoll** on client socket — no spurious wakeups at high data rates
- **Box-Muller + Xoshiro256++** — ~2× faster than `std::normal_distribution`
- **Power-of-two ring buffer** — modulo via bitwise AND, no branch in wrap logic
- **Histogram percentiles** — O(512) percentile queries, fits in L1 cache
- **Single `write()` per display frame** — no partial-update flicker

See `docs/` for full design documentation.
