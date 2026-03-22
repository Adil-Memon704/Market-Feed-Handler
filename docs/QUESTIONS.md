# QUESTIONS.md — Answers to Critical Thinking Questions

## Section 1: Exchange Simulator

**Q1. How do you efficiently broadcast to multiple clients without blocking?**

The broadcast thread uses non-blocking sockets exclusively. Every client fd is set to `O_NONBLOCK` at `accept()` time. The broadcast loop calls `send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT)` for each client. If `send()` returns immediately with `EAGAIN`, the message is queued in a per-client spill buffer and the fd is registered for `EPOLLOUT`; the broadcast_thread continues to the next client without waiting.

This means the broadcast loop's worst-case per-client cost is a single failed `send()` syscall (~200 ns), not the full round-trip time to the client. Total broadcast cost to 100 clients with no backpressure is approximately `100 × 1.2 µs = 120 µs` — entirely acceptable at 10K msg/s. At 100K msg/s (100 µs per message), the broadcast loop must complete within 100 µs, which it does comfortably for up to ~80 clients.

**Q2. What happens when a client's TCP send buffer fills up?**

When the kernel send buffer is full, `send()` on a non-blocking socket returns `-1` with `errno = EAGAIN`. The server responds in two stages:

- **Stage 1 (soft):** The unsent message is queued in the per-client in-process spill buffer. The fd is registered for `EPOLLOUT`. When the kernel drains the socket's send buffer (the client reads data), `EPOLLOUT` fires and the server flushes the spill queue.
- **Stage 2 (hard):** If the spill buffer exceeds 64 MB (the server's slow-client threshold), the connection is forcibly closed (`close(fd)`). This mirrors how real exchange co-location feeds behave — the exchange does not buffer indefinitely for lagging clients. The client is expected to reconnect and miss the gap.

The key insight is that **the server must never block waiting for any single client**, because that would stall data delivery to all other clients.

**Q3. How do you ensure fair distribution when some clients are slower?**

"Fairness" in a broadcast feed is typically **not** the goal — the exchange sends a message once and every client either receives it or doesn't. However, to avoid fast clients being penalised by slow clients sharing the same broadcast loop, the following mechanisms are used:

- The broadcast loop iterates all clients in a fixed order each tick. Slow clients (in the spill-queue phase) get a fast `pending_sends.push_back()` call instead of a `send()` syscall — approximately the same cost either way.
- Slow clients are disconnected after exceeding the spill buffer limit, so they cannot hold up the loop indefinitely.
- For extreme fairness (e.g. where some clients must receive every message before others), a per-client dedicated write thread or io_uring could be used. For a market data feed, this is unnecessary — the latency requirement is about being fast for all clients, not about equal treatment.

**Q4. How would you handle 1000+ concurrent client connections?**

Several architectural changes are needed at this scale:

1. **io_uring with registered buffers:** Instead of per-fd `send()` syscalls, io_uring allows submitting `IORING_OP_SEND` operations for all 1000 fds in a single `io_uring_enter()` syscall, with the kernel completing them asynchronously. This reduces syscall overhead from O(N) to O(1).
2. **Sharded broadcast threads:** Divide clients into groups of 250. Each group has a dedicated broadcast thread, with the tick generator posting messages to N SPSC queues (one per group) using `memcpy` — the cost of N copies is justified by linear broadcast parallelism.
3. **Memory-mapped send buffers:** For the highest-throughput scenario, each client's pending send data could live in a shared-memory ring that the kernel copies directly, avoiding the userspace-to-kernel copy in `send()`.
4. **CPU affinity:** Pin each broadcast thread to a dedicated core, preventing OS scheduler interference.

---

## Section 2: TCP Client Socket Implementation

**Q1. Why use epoll edge-triggered instead of level-triggered for the feed handler?**

With level-triggered (LT) mode, `epoll_wait` keeps returning `EPOLLIN` on every call as long as there is unread data in the socket buffer. At 100K msg/s, the socket buffer always has data, meaning `epoll_wait` would return immediately on every call — effectively becoming a busy-wait loop consuming 100% CPU just for the epoll check.

With edge-triggered (ET) mode, `EPOLLIN` fires exactly once per new data arrival (per kernel buffer transition from empty to non-empty). The application is required to drain the buffer completely each time (loop `recv()` until `EAGAIN`). This is the correct pattern for the feed handler anyway — we want to empty the buffer as fast as possible — and it allows `epoll_wait` to legitimately block when no data is available, surrendering the CPU to other threads.

In summary: ET eliminates spurious wakeups at high data rates, reduces CPU usage, and naturally enforces the drain-to-completion pattern that minimises buffering latency.

**Q2. How do you handle the case where recv() returns EAGAIN/EWOULDBLOCK?**

`EAGAIN` from `recv()` on an ET-registered socket means the kernel buffer has been completely drained. The correct response is to **break out of the recv loop and return to epoll_wait**. No error is logged; this is the expected termination condition of the drain loop.

```cpp
while (true) {
    ssize_t n = recv(fd, buf, len, 0);
    if (n > 0)      { process(buf, n); continue; }
    if (n == 0)     { on_disconnect(); break; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // fully drained — normal
    if (errno == EINTR) continue;                         // signal — retry
    on_error(errno); break;                               // real error
}
```

One subtle point: after breaking on `EAGAIN`, the application must not assume the buffer stays empty — the kernel may add more data between the `EAGAIN` return and the next `epoll_wait`. The `epoll_wait` will correctly fire again when that happens.

**Q3. What happens if the kernel receive buffer fills up?**

If `SO_RCVBUF` (4 MB) fills up because the application is not calling `recv()` fast enough, the kernel begins **dropping incoming TCP segments** and sending zero-window advertisements to the sender. The sender (exchange simulator) will reduce its send rate to the window size — zero — and stall.

This has two consequences:
1. The client effectively **pauses the data feed** — the server cannot send more data until the client drains its buffer.
2. Sequence gaps are **not** caused (TCP guarantees in-order delivery within the connection), but other clients may experience higher latency if they share a broadcast loop with the stalled client.

The application-level protection is the 8 MB application ring buffer — if the parse thread keeps up with the network thread (which it does at up to 1.85M msg/s), the kernel buffer never fills. The ring buffer provides 2.3 seconds of absorption time at maximum feed rate.

**Q4. How do you detect a silent connection drop (no FIN/RST)?**

Three mechanisms work in concert:

1. **TCP keepalives:** Set `TCP_KEEPIDLE=10s`, `TCP_KEEPINTVL=5s`, `TCP_KEEPCNT=3`. After 10 seconds of silence, the kernel sends keepalive probes every 5 seconds. After 3 unanswered probes (25 seconds total), the kernel closes the socket and generates `EPOLLHUP`/`EPOLLERR` on the epoll fd — triggering reconnection.
2. **Application heartbeats:** The server sends a `Heartbeat (0x03)` message every 1 second. The client checks `last_recv_time` every 5 seconds via a timer fd. If no message has been received in 30 seconds, the client proactively closes and reconnects.
3. **EPOLLRDHUP:** Registered alongside `EPOLLIN`. Fires when the peer performs a half-close (sends FIN). Used for clean shutdown detection even when the peer's OS crashes and doesn't send RST.

In practice, the heartbeat mechanism (mechanism 2) is the fastest detector — it triggers within 30 seconds vs 25 seconds for keepalive. For ultra-low latency detection, the timeout could be reduced to 3–5 seconds.

**Q5. Should reconnection logic be in the same thread or separate?**

**Separate thread.** The reconnection logic involves:
- `connect()` with a timeout (potentially blocks for `timeout_ms = 5000`)
- Sleeping for the backoff delay (up to 30 seconds)
- DNS resolution (if hostname is used instead of IP)

If all of this runs in the network thread, the epoll event loop is blocked for the entire reconnect attempt, including the sleep. During this time, if the connection succeeds and data arrives, the epoll loop cannot process it.

The correct design is a `reconnect_thread` that:
1. Sleeps for the backoff duration.
2. Calls `connect()` (blocking with a short timeout, or non-blocking with a `connect()` + `EPOLLOUT` wait).
3. Posts the new fd to the network thread via an `eventfd` or `pipe`.

The network thread receives the new fd, registers it with epoll, and resumes normal operation — all without blocking its event loop.

---

## Section 3: Binary Protocol Parser

**Q1. How do you buffer incomplete messages across multiple recv() calls efficiently?**

The parser maintains a **power-of-two ring buffer** in application memory. The network thread writes received bytes into the ring buffer; the parse thread reads from it independently. No copying occurs between the two threads — the ring buffer IS the parse buffer.

The parse thread tracks a `read_pos` pointer. When it needs a complete message:
1. Check if `write_pos - read_pos >= HEADER_SIZE (16)`. If not, yield and wait.
2. Peek at the header to determine the payload size.
3. Check if `write_pos - read_pos >= HEADER_SIZE + payload_size`. If not, yield and wait.
4. If the message wraps around the ring buffer end, copy it to a small `scratch[64]` buffer (the only copy in the hot path).
5. Parse in-place. Advance `read_pos`. No allocation.

This approach means incomplete messages require zero memory movement — the bytes simply sit in the ring buffer until the rest of the message arrives.

**Q2. What happens when you detect a sequence gap — drop it or request retransmission?**

**Drop and continue.** Market data feeds over TCP are best-effort at the application level, even though TCP guarantees byte-stream integrity. Sequence gaps arise not from TCP reordering (TCP prevents that) but from:

- **Deliberate injection** by the simulator (1% gap rate in tests).
- **Server-side drops** when a slow client was disconnected and reconnected — it rejoined mid-stream.

Retransmission is not feasible because:
1. There is no upstream retransmit channel in this architecture.
2. By the time a retransmit could arrive, the missed tick is stale — acting on it would be trading on old data, which is worse than skipping it.
3. Real NSE feeds (e.g. MCAST) have sequence gaps routinely; downstream strategies are designed to handle them.

The correct response: log the gap size (`expected_seq - received_seq`), increment the `gap_counter` metric, update `expected_seq` to the received value, and continue parsing. Gap monitoring is used operationally to detect feed health degradation.

**Q3. How would you handle messages arriving out of order? (If TCP guarantees order, when might this happen?)**

TCP guarantees in-order delivery within a single connection. Out-of-order messages at the application level can still occur in the following scenarios:

- **Reconnection:** After disconnecting and reconnecting, the client receives a new sequence starting from the server's current sequence number. The "old" sequence is gone; the first message on the new connection has a higher sequence number than the last message received before disconnect — appearing as a large gap, not out-of-order.
- **Multiple connections to the same feed (e.g. primary + backup):** If a backup feed delivers a message that was already received on the primary, it arrives as a duplicate with a lower sequence number. These should be detected and discarded.
- **Simulator bug:** If the simulator accidentally sends sequence numbers non-monotonically, the parser detects `received_seq < expected_seq` and discards the message as a duplicate.

Handling: maintain `last_seen_seq` per symbol. If `received_seq <= last_seen_seq`, discard silently (duplicate). If `received_seq > last_seen_seq + 1`, record a gap.

**Q4. How do you prevent buffer overflow with malicious large message lengths?**

The parser validates the message length **before** attempting to read the payload:

```cpp
uint16_t msg_type = read_u16(header);
size_t payload_size;

switch (msg_type) {
    case 0x01: payload_size = 12; break;   // Trade: 8+4 bytes
    case 0x02: payload_size = 24; break;   // Quote: 8+4+8+4 bytes
    case 0x03: payload_size = 0;  break;   // Heartbeat: no payload
    default:
        // Unknown type — could be a malformed or malicious message
        // Cannot determine payload size; disconnect to avoid de-sync
        on_disconnect(DisconnectReason::PROTOCOL_ERROR);
        return;
}

// Maximum total message size is 16 (header) + 24 (max payload) = 40 bytes
// No length field exists in the protocol — size is fully determined by type
// Therefore there is NO length field to overflow with a malicious value
```

Since the message format uses a fixed-length type field to determine payload size (not an explicit length field), there is no attack surface for a length-based overflow. The only defence needed is against an unknown message type, which triggers disconnection. If the protocol were extended to include a length field, it must be validated against `MAX_MSG_SIZE = 64` (generous upper bound) before any buffer operation.

---

## Section 4: Lock-Free Symbol Cache

**Q1. How do you prevent readers from seeing inconsistent state during updates?**

A **seqlock (sequence lock)** is used. The writer increments a sequence counter to an odd value at the start of the update and back to an even value at the end. Readers check the counter before and after copying the state; if the values differ or the counter is odd (write in progress), they retry. This ensures readers never see a partially-written state.

The key property: **writers never block for readers, and readers never block for writers** (readers just retry). This is ideal for the single-writer/single-reader pattern here.

**Q2. What memory ordering do you need for atomic operations?**

| Operation | Ordering | Reason |
|-----------|----------|--------|
| Writer: seq store (odd — mark dirty) | `memory_order_release` | Ensures all subsequent state writes are not reordered before this store. A reader that sees `seq = odd` will not also see the old state data. |
| Writer: seq store (even — mark clean) | `memory_order_release` | Ensures all state writes are committed before the "clean" marker is visible to readers. |
| Reader: seq load (first, before copy) | `memory_order_acquire` | Pairs with writer's release — ensures the reader sees the full state written before the seq update. |
| Reader: seq load (second, after copy) | `memory_order_acquire` | Ensures the state copy completed before this load — prevents the compiler from reordering the copy after the verification load. |

`memory_order_seq_cst` would also be correct but adds unnecessary full memory barriers. The acquire/release pair is the minimum needed.

**Q3. How do you handle cache line bouncing with single writer, visualization reader?**

Cache line bouncing occurs when two cores write to different locations on the same cache line (false sharing), causing the cache line to "bounce" between their L1 caches via the coherency protocol.

Prevention via `alignas(64)` on each `CacheEntry`:

```cpp
struct alignas(64) CacheEntry {
    std::atomic<uint32_t> seq;
    char _pad[60];    // Pad seq to its own cache line
    MarketState state; // State begins at byte 64 (own cache line)
};

// Array layout: each CacheEntry starts at a 64-byte boundary
CacheEntry cache_[500];
```

With this layout:
- The writer (parse_thread on core 0) owns the `state` cache line exclusively during updates — no reader touches `state` during the write window (enforced by the seqlock protocol).
- The reader (visualize_thread on core 1) reads `seq` and `state` in sequence. The `seq` cache line bounces between cores on every read, but this is unavoidable and costs ~60 ns per snapshot — acceptable since the visualizer only runs every 500 ms.

**Q4. Do you need read-copy-update (RCU) pattern here?**

No. RCU is appropriate when:
1. Reads vastly outnumber writes (millions of reads per write).
2. The protected object is large (a complex struct or a linked list).
3. Low-overhead reclamation of old versions is needed (pointer-based structures).

This cache has a single writer updating at 100K/s and a single reader (visualizer) reading at 2/s (every 500 ms). The write rate dominates. RCU would add:
- A grace-period mechanism to reclaim old versions.
- Pointer indirection for each read (cache unfriendly).
- Complexity in the writer (allocate new version, swap pointer, wait for grace period).

The seqlock is simpler, faster, and perfectly suited to this workload. RCU would provide value only if there were hundreds of reader threads or if the MarketState struct were an order book with 20+ price levels.

---

## Section 5: Terminal Visualization

**Q1. How do you update display without interfering with network/parsing threads?**

The visualizer is a **completely independent thread** that only interacts with the rest of the system through two interfaces:

1. **SymbolCache reads:** Lock-free `getSnapshot()` calls. The seqlock ensures these are non-blocking and add negligible overhead to the writer.
2. **LatencyStats reads:** An `atomic<LatencyStats>` snapshot is posted by the tracker thread every 1 second; the visualizer reads it with `memory_order_acquire`. No mutex involved.

The visualizer thread calls `clock_nanosleep` to wake every 500 ms, builds the full screen string in a local buffer, and emits it with a single `write()` to stdout. The only shared state is the SymbolCache (lock-free) and the LatencyStats (atomic). The visualizer cannot block the network thread or parse thread under any circumstance.

**Q2. Should you use ncurses or raw ANSI codes? Why?**

Raw ANSI codes are used. The decision factors:

- **No dependency:** ncurses adds a library dependency with complex initialisation (`initscr()`, `endwin()`, signal handling for resize). ANSI codes work in any VT100-compatible terminal with zero setup.
- **Thread safety:** ncurses is not thread-safe by default. The ncurses `WINDOW` must be accessed from a single thread. If a crash leaves the terminal in raw mode, the terminal becomes unusable. With ANSI codes, the worst case is garbled output easily fixed with `reset`.
- **Single write per frame:** ANSI codes allow building the entire frame as a single string and flushing it in one `write()`. ncurses makes individual calls per cell, which can cause visible partial-update flicker.
- **Simplicity:** The display is a fixed 20-row table — the full power of ncurses (windows, panels, forms) is unnecessary.

The one ncurses advantage foregone is robust `SIGWINCH` (terminal resize) handling. Handled instead by reading the new terminal size from `ioctl(STDOUT_FILENO, TIOCGWINSZ)` on a `SIGWINCH` signal, then truncating or padding the display to fit.

**Q3. How do you calculate percentage change when prices update continuously?**

The percentage change is calculated relative to the **session open price** — the first price seen by the visualizer for each symbol since the process started. This is stored in the visualizer thread's local state (not in the shared cache) and is never reset during the session.

```cpp
// Only in visualizer_thread — no concurrency needed
double open_price[500] = {};
bool   open_set[500]   = {};

double pct_change(uint16_t id, double current) {
    if (!open_set[id]) {
        open_price[id] = current;
        open_set[id]   = true;
        return 0.0;
    }
    return (current - open_price[id]) / open_price[id] * 100.0;
}
```

This approach avoids the "reference price drift" problem that would occur if the change were calculated relative to the previous tick — a stock that rises steadily would show +0.001% every tick, which is unhelpful. The session-open reference gives the trader intuitive intraday context.

---

## Section 7: Performance Measurement

**Q1. Sorting is O(n log n) — how can you calculate percentiles faster?**

Instead of sorting, a **bucketed histogram** is used. Sample values are placed into pre-defined latency buckets at record time. Percentile calculation scans the buckets in O(B) where B is the number of buckets (512 in this implementation) — effectively O(1) since B is constant and small (fits in L1 cache).

```
Record a sample of 47 ns → increment bucket[47] by 1.   O(1)

Compute p99 (sample_count=1,000,000):
  target = 0.99 × 1,000,000 = 990,000
  Scan buckets from 0 upward, accumulating counts.
  When accumulated >= 990,000, return bucket's latency range.
  O(512) comparisons — ~1 µs total.
```

Accuracy: the histogram provides the bucket containing the percentile, not the exact value. Bucket widths range from 1 ns at the low end to 4096 ns at the high end. For p99 in the 10–100 µs range, the bucket width is 512 ns — error is at most 512 ns, acceptable for operational monitoring.

**Q2. How do you minimize the overhead of timestamping?**

Several techniques are combined:

1. **VDSO for `clock_gettime`:** On Linux, `clock_gettime(CLOCK_MONOTONIC)` is implemented in the VDSO — a page of kernel code mapped into every process's address space. It executes without a syscall (no ring-0 transition), costing ~5 ns instead of ~200 ns for a full syscall.
2. **`RDTSC` for sub-nanosecond resolution:** Where only a cycle count is needed (not a wall-clock time), `__rdtsc()` is used directly (~1 ns overhead). The TSC is converted to nanoseconds using the pre-measured CPU frequency: `ns = cycles × ns_per_cycle`.
3. **Batch timestamping:** Instead of timestamping every message individually, the network thread records `recv_time` once per `recv()` call and assigns that timestamp to all messages parsed from that batch. This introduces up to ~1 µs of timestamp imprecision but reduces the timestamping cost by N× (where N is messages per recv batch).
4. **Avoid `gettimeofday`:** This older syscall is not VDSO-accelerated on all kernels and has only microsecond resolution. Always use `clock_gettime` instead.

**Q3. What granularity of histogram buckets balances accuracy vs memory?**

The bucket scheme used is a **logarithmic-ish hybrid**:

| Bucket range | Bucket width | Count | Memory |
|-------------|-------------|-------|--------|
| 0–63 ns | 1 ns | 64 | 512 B |
| 64 ns–1 µs | 8 ns | 120 | 960 B |
| 1–10 µs | 64 ns | 141 | 1,128 B |
| 10–100 µs | 512 ns | 176 | 1,408 B |
| 100 µs–1 ms | 4096 ns | 220 | 1,760 B |
| **Total** | | **~720 buckets** | **~5.7 KB** |

This fits comfortably in L1 cache (32–64 KB), meaning histogram reads for percentile queries are essentially free. The 1-ns granularity at the low end captures the sub-10 ns cache update measurements precisely. The 4096-ns granularity at the high end is sufficient for reporting tail latencies (p999) where precision to the nearest 4 µs is acceptable.

Alternative approaches:
- **HDR Histogram:** Sub-1% relative accuracy across all magnitudes, but requires ~48 KB per histogram. Excellent for production monitoring; overbuilt for this use case.
- **t-Digest:** Streaming percentile algorithm with configurable accuracy. More complex to implement; better suited to very long-running streams where memory must be bounded.
- **Exact sort:** Accurate to the nanosecond but O(N log N) and requires storing all 1M samples (~8 MB). Used only for one-time calibration runs, not for online monitoring.
