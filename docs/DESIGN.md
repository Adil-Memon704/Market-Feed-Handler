# DESIGN.md — System Architecture & Design Decisions

## Table of Contents
1. [System Architecture](#1-system-architecture)
2. [Geometric Brownian Motion](#2-geometric-brownian-motion)
3. [Network Layer Design](#3-network-layer-design)
4. [Memory Management Strategy](#4-memory-management-strategy)
5. [Concurrency Model](#5-concurrency-model)
6. [Visualization Design](#6-visualization-design)
7. [Performance Optimization](#7-performance-optimization)

---

## 1. System Architecture

### 1a. Client–Server Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                  Exchange Simulator (Server)                  │
│                                                              │
│  ┌─────────────┐    ┌──────────────┐    ┌────────────────┐  │
│  │ TickGenerator│───▶│ MessageQueue │───▶│ BroadcastEngine│  │
│  │  (GBM + RNG) │    │ (ring buffer)│    │  (epoll loop)  │  │
│  └─────────────┘    └──────────────┘    └───────┬────────┘  │
│         ▲                                        │           │
│  ┌──────┴──────┐                       ┌────────▼────────┐  │
│  │  100 Symbol │                       │  ClientManager  │  │
│  │  Processes  │                       │  (fd table +    │  │
│  └─────────────┘                       │   send buffers) │  │
│                                        └─────────────────┘  │
└────────────────────────────────────┬─────────────────────────┘
                                     │  Binary Protocol over TCP
                        ┌────────────▼────────────────────────┐
                        │        Feed Handler (Client)         │
                        │                                      │
                        │  ┌────────────┐  ┌───────────────┐  │
                        │  │SocketLayer │─▶│ BinaryParser  │  │
                        │  │(epoll, ET) │  │ (zero-copy,   │  │
                        │  └────────────┘  │  ring buffer) │  │
                        │                  └──────┬────────┘  │
                        │  ┌─────────────┐        │           │
                        │  │ Visualizer  │  ┌─────▼─────────┐ │
                        │  │ (500ms tick)│◀─│ SymbolCache   │ │
                        │  └─────────────┘  │ (lock-free,   │ │
                        │                   │  seqlock)     │ │
                        │  ┌─────────────┐  └───────────────┘ │
                        │  │LatencyTracker│                    │
                        │  │(ring buffer) │                    │
                        │  └─────────────┘                    │
                        └──────────────────────────────────────┘
```

### 1b. Thread Model

**Server Threads:**

| Thread | Role |
|--------|------|
| `accept_thread` | Accepts incoming TCP connections, registers fds with epoll |
| `tick_thread` | Runs GBM for all 100 symbols at the configured tick rate, posts messages to a lock-free SPSC queue |
| `broadcast_thread` | Drains the SPSC queue and writes to all connected client send buffers via epoll edge-triggered loop |

**Client Threads:**

| Thread | Role |
|--------|------|
| `network_thread` | epoll event loop — reads socket, feeds raw bytes to the parser ring buffer |
| `parse_thread` | Drains parser ring buffer, validates messages, updates SymbolCache and LatencyTracker |
| `visualize_thread` | Reads SymbolCache snapshots every 500 ms, renders ANSI terminal display |

Keeping the parse thread separate from the network thread ensures that a slow visualization or checksum operation never stalls socket reads. The network thread only copies bytes from kernel space; it never blocks.

### 1c. Data Flow — Tick Generation to Terminal Display

```
T0: GBM formula runs in tick_thread
    → price + bid/ask spread computed
    → message serialised into fixed-size binary struct

T1: Message posted to SPSC ring buffer (broadcast_thread picks it up)
    → sequence number assigned (atomic fetch_add)
    → nanosecond timestamp via clock_gettime(CLOCK_REALTIME)

T2: broadcast_thread iterates client_fd list
    → writev() or send() to each socket (non-blocking)
    → slow clients detected when send() returns EAGAIN twice

T3: Client network_thread receives bytes via epoll
    → recv() into pre-allocated ring buffer (4MB SO_RCVBUF)
    → notifies parse_thread (eventfd or condition variable)

T4: Parser reconstructs message from ring buffer
    → validates sequence number & XOR checksum
    → calls cache.update(symbol_id, market_state)

T5: Visualizer reads cache.getSnapshot() every 500 ms
    → renders ANSI table to stdout
```

End-to-end latency target: **T0 → T4 < 50 µs** at localhost.

---

## 2. Geometric Brownian Motion

### 2a. Mathematical Formulation

Stock prices are modelled as a continuous-time stochastic process governed by the SDE:

```
dS = μ S dt + σ S dW
```

where `dW = ε √dt` and `ε ~ N(0,1)`.

The discrete-time (Euler–Maruyama) approximation used for each tick:

```
S(t + Δt) = S(t) · exp( (μ - σ²/2) Δt + σ · ε · √Δt )
```

The exponential form is preferred over the naive Euler form because it guarantees `S > 0` for all time and produces a log-normal price distribution, consistent with real equity behaviour.

### 2b. Implementation Approach

**Box–Muller Transform** (used instead of std::normal_distribution to avoid hidden branching):

```cpp
double z0, z1;
double u1 = uniform_real(rng);   // U(0,1) — use Xoshiro256++
double u2 = uniform_real(rng);
z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
z1 = sqrt(-2.0 * log(u1)) * sin(2.0 * M_PI * u2);
// z0 and z1 are both standard normal; use both to avoid wasting samples
```

Per-symbol state maintained:

```cpp
struct SymbolProcess {
    double price;          // current mid price
    double mu;             // drift (randomised at init)
    double sigma;          // volatility in [0.01, 0.06]
    double spread_pct;     // bid-ask spread in [0.0005, 0.002]
    uint64_t seq;          // next sequence number
};
```

### 2c. Parameter Choices

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `μ` | `U(-0.02, +0.02)` per symbol | Neutral market with slight per-symbol drift |
| `σ` | `U(0.01, 0.06)` per symbol | Covers low-vol blue chips to high-vol small caps |
| `Δt` | `1 / tick_rate` (e.g. 0.00001 s at 100 K/s) | Matches message emission frequency |
| Initial price | `U(100, 5000)` | NSE price range — ₹100 (illiquid mid-cap) to ₹5000 (HDFC Bank) |
| Spread | `price × U(0.0005, 0.002)` | 0.05–0.2% of mid; widens proportionally |

### 2d. Ensuring Realistic Price Movements

- **Floor guard:** if generated price < 1.0, reset to initial price (avoids zero/negative prices in extreme σ scenarios).
- **Volume correlation:** `quantity ~ Poisson(λ)` where `λ` increases when `|ε| > 1.5` (high-volatility ticks attract more volume).
- **Intraday drift:** `μ` is modulated by a sine wave over a simulated 6.5-hour session to mimic opening rush, midday lull, and closing auction.
- **Message mix:** 70% quotes, 30% trades, matching typical NSE feed ratios.

---

## 3. Network Layer Design

### 3a. Server: epoll-based Multi-Client Handling

The server uses a single `epoll` instance with **level-triggered** mode for accepted client sockets. Level-triggered is chosen on the server because:

- The broadcast loop writes to every client in a tight loop; partial sends are retried immediately rather than waiting for the next epoll event.
- Edge-triggered would require the server to drain each fd completely before returning to the broadcast loop, which increases per-write latency.

```
epoll_create1(EPOLL_CLOEXEC)
  → EPOLLIN on listen_fd   (new connections)
  → EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP on each client_fd
```

The `EPOLLOUT` event is used purely for flow control: if a `send()` returns `EAGAIN`, the fd is registered for `EPOLLOUT` and the pending message is queued in a per-client send buffer until the socket becomes writable again.

### 3b. Buffer Management

Each connected client has a dedicated `SendBuffer`:

```cpp
struct ClientState {
    int fd;
    std::deque<std::vector<uint8_t>> pending_sends;  // spill queue
    uint64_t bytes_sent;
    uint64_t messages_dropped;   // incremented on overflow
    steady_clock::time_point last_active;
};
```

A client is considered **slow** if its `pending_sends` queue exceeds 64 MB. At that point, the server disconnects it and logs a warning — this is consistent with how real exchange co-location feeds behave (no retransmit, buyer beware).

### 3c. Reconnection Logic and Backoff Strategy

The client uses **exponential backoff with jitter**:

```
delay = min(base_delay × 2^attempt, max_delay) + jitter
base_delay = 100 ms, max_delay = 30 s, jitter = U(0, 100 ms)
```

State machine:

```
DISCONNECTED → CONNECTING → CONNECTED → SUBSCRIBING → LIVE
      ↑_________________________________________________|
                    (any error or FIN/RST)
```

### 3d. Flow Control Mechanism

- **Server side:** `send()` is non-blocking. If it returns `-1 (EAGAIN)`, data goes into the per-client spill queue. If the spill queue exceeds the threshold, the client is dropped.
- **Client side:** `SO_RCVBUF` is set to 4 MB. The network thread reads as fast as possible in the epoll loop, posting to the parser ring buffer. If the ring buffer is full (parser thread is too slow), the oldest slot is overwritten and a `ring_overflow` counter is incremented — a best-effort approach appropriate for market data (stale data is worse than dropped data).

---

## 4. Memory Management Strategy

### 4a. Buffer Lifecycle

```
Kernel receive buffer (4 MB SO_RCVBUF)
    │
    │ recv() — single copy into pre-allocated RecvBuffer
    ▼
RecvBuffer (ring buffer, 8 MB, power-of-two size)
    │
    │ Parser reads directly — zero additional copy
    ▼
MessageView (pointer + length into RecvBuffer)
    │
    │ Cache update writes only scalar fields — no heap allocation
    ▼
SymbolCache[symbol_id] (seqlock-protected struct)
```

The goal is **one memory copy total**: kernel buffer → RecvBuffer. The parser and cache update work entirely on the bytes already sitting in the RecvBuffer.

### 4b. Allocation Patterns

- **Hot path:** zero heap allocation. All buffers (RecvBuffer, per-client SendBuffer, LatencyTracker histogram) are allocated once at startup and reused for the process lifetime.
- **Cold path (connection accept):** `new ClientState` is acceptable here since it happens at most once per connection event.
- **Server tick generation:** GBM parameters and price state are in a flat `std::array<SymbolProcess, 100>` — cache-friendly sequential layout.

### 4c. Alignment and Cache Considerations

- `SymbolProcess` and `MarketState` structs are aligned to **64 bytes** (one cache line) using `alignas(64)`.
- The `SymbolCache` array is allocated on a 64-byte boundary via `posix_memalign` to prevent false sharing between adjacent symbol entries during concurrent writer/reader access.
- `LatencyTracker` histogram buckets are stored in a separate cache line from the ring buffer write pointer to avoid false sharing between the recording thread and the stats-reading thread.

### 4d. Pool Usage

A `MemoryPool<T>` (slab allocator) is used for:
- Parser `MessageView` objects (avoid repeated `new`/`delete` in the parse loop).
- Per-client `SendBuffer` nodes on the server.

The pool is a lock-free free-list implemented with `std::atomic<Node*>` CAS operations, safe for single-producer/single-consumer usage patterns.

---

## 5. Concurrency Model

### 5a. Lock-Free Techniques

**SymbolCache — Seqlock (sequence lock):**

```cpp
struct alignas(64) CacheEntry {
    std::atomic<uint32_t> seq{0};   // odd = write in progress
    MarketState state;
};

// Writer:
void update(uint16_t id, const MarketState& s) {
    auto& e = entries_[id];
    uint32_t old = e.seq.load(memory_order_relaxed);
    e.seq.store(old + 1, memory_order_release);  // mark dirty
    e.state = s;                                  // non-atomic write
    e.seq.store(old + 2, memory_order_release);  // mark clean
}

// Reader:
MarketState getSnapshot(uint16_t id) {
    auto& e = entries_[id];
    MarketState s;
    uint32_t seq1, seq2;
    do {
        seq1 = e.seq.load(memory_order_acquire);
        if (seq1 & 1) { cpu_pause(); continue; }  // writer active
        s = e.state;
        seq2 = e.seq.load(memory_order_acquire);
    } while (seq1 != seq2);
    return s;
}
```

This gives readers **sub-50 ns** average latency with no CAS contention.

**LatencyTracker — atomic ring buffer:**  
The write index is an `atomic<uint64_t>` incremented with `fetch_add(1, relaxed)`. Each slot is independently written, so no CAS loop is needed.

**SPSC Queue (tick_thread → broadcast_thread):**  
A power-of-two ring buffer with separate `atomic<uint64_t>` head and tail, accessed with `relaxed` ordering by the producer and `acquire/release` between producer and consumer. No locks needed for single-producer/single-consumer.

### 5b. Memory Ordering Choices

| Operation | Ordering | Reason |
|-----------|----------|--------|
| Seqlock write (seq increment) | `release` | Ensure state writes are visible before seq is updated |
| Seqlock read (seq load) | `acquire` | Pair with writer's release to see complete state |
| LatencyTracker record | `relaxed` | Only one thread writes; ordering only needed for stats snapshot |
| SPSC queue tail publish | `release` | Consumer must see the written slot |
| SPSC queue head read | `acquire` | Paired with producer's release |

### 5c. Single-Writer Multiple-Reader Pattern

The architecture is intentionally **single-writer** per symbol (the parse thread). This eliminates write–write races entirely. The seqlock is therefore non-contended on the write side and approaches the cost of a plain store + two atomic seq updates (~8 ns on modern x86).

### 5d. RCU Consideration

RCU would be warranted if the MarketState struct were large (e.g. an order book with 10+ levels) or if the write frequency were so high that seqlock spinning became measurable. For a 48-byte `MarketState` updated at 100K/s with a single reader (visualizer), the seqlock is simpler and sufficient. RCU would add deferred reclamation complexity without latency benefit here.

---

## 6. Visualization Design

### 6a. Update Strategy

The visualizer runs in a dedicated thread that wakes every **500 ms** via `clock_nanosleep`. It:
1. Takes a snapshot of the top-20 most active symbols from the SymbolCache (sorted by `update_count` descending).
2. Builds the full screen buffer in a local `std::string`.
3. Writes the entire buffer in a single `write()` system call — minimising the number of partial-screen flicker frames.

The display thread never touches the network or parser data structures directly; it only calls `cache.getSnapshot()`.

### 6b. Raw ANSI Codes vs ncurses

**Raw ANSI codes** are chosen over ncurses for the following reasons:

| Concern | ANSI codes | ncurses |
|---------|-----------|---------|
| Dependencies | None | libncurses |
| Thread safety | Safe (single write per frame) | Complex (not thread-safe by default) |
| Latency | Single `write()` per frame | Multiple library calls with internal locking |
| Portability | Works in any VT100-compatible terminal | Requires terminfo database |
| Code complexity | ~100 lines | ~300 lines + init/teardown |

The trade-off is that ncurses handles terminal resize and complex cursor movement more robustly. For this use case (fixed 20-row table), raw ANSI is entirely sufficient.

### 6c. Percentage Change Calculation

```cpp
// Stored per symbol in the visualizer (not in SymbolCache):
double open_price_[500];    // Price at the start of the session
bool   open_set_[500];      // Whether open has been recorded

double pct_change(uint16_t id, double current) {
    if (!open_set_[id]) {
        open_price_[id] = current;
        open_set_[id] = true;
        return 0.0;
    }
    return (current - open_price_[id]) / open_price_[id] * 100.0;
}
```

The open price is captured on the first update seen by the visualizer (approximates the session open). This state lives only in the visualizer thread — no synchronisation needed.

---

## 7. Performance Optimization

### 7a. Hot Path Identification

The critical path (highest frequency, lowest latency required) is:

```
recv() bytes → ring buffer write → parser reads bytes → cache.update()
```

Everything else (visualizer, reconnect logic, stats CSV export) is off the hot path.

### 7b. Cache Optimization Techniques

- **Sequential symbol layout:** `SymbolProcess[100]` is a plain array. The GBM tick loop iterates it in order, favouring hardware prefetcher.
- **Struct packing:** `MarketState` is ordered by field size (doubles first, then uint32s, then uint64s) to avoid internal padding and stay within 2 cache lines.
- **False sharing prevention:** Each `CacheEntry` is `alignas(64)`. Adjacent symbols do not share a cache line even if the visualizer reads symbol N while the parser updates symbol N+1.

### 7c. False Sharing Prevention

```cpp
// BAD — all fields on same cache line, writer and reader contend:
struct CacheEntry { uint32_t seq; MarketState state; };

// GOOD — seq and state on separate cache lines:
struct alignas(64) CacheEntry {
    std::atomic<uint32_t> seq;
    char _pad[60];
    MarketState state;   // starts at byte 64
};
```

### 7d. System Call Minimization

- **Batched sends:** On the server, `writev()` is used to combine the message header and payload into a single syscall.
- **Large SO_SNDBUF / SO_RCVBUF:** 4 MB receive buffer on the client reduces the frequency of `recv()` calls needed to drain the kernel buffer.
- **`CLOCK_REALTIME` with `VDSO`:** On Linux, `clock_gettime(CLOCK_REALTIME)` is resolved via the VDSO and does not require a syscall — making nanosecond timestamping effectively free.
- **`TCP_NODELAY`:** Disables Nagle's algorithm so small messages (32–48 bytes) are sent immediately without waiting for a full MTU segment.
- **Batch epoll events:** `epoll_wait(epfd, events, 64, -1)` retrieves up to 64 events per wakeup, amortising the syscall overhead across multiple fds.
