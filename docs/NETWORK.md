# NETWORK.md — Socket Implementation Details

## Table of Contents
1. [Server-Side Design](#1-server-side-design)
2. [Client-Side Design](#2-client-side-design)
3. [TCP Stream Handling](#3-tcp-stream-handling)
4. [Connection Management](#4-connection-management)
5. [Error Handling](#5-error-handling)

---

## 1. Server-Side Design

### 1a. Multi-Client epoll Handling

The server creates a single epoll instance at startup and manages all file descriptors — the listen socket and all connected client sockets — through it.

```
epoll_create1(EPOLL_CLOEXEC)

listen_fd:
  → fcntl(O_NONBLOCK)
  → bind(), listen(SOMAXCONN)
  → epoll_ctl(EPOLL_CTL_ADD, EPOLLIN)

On EPOLLIN for listen_fd:
  → accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)
  → setsockopt(TCP_NODELAY, SO_SNDBUF=4MB)
  → epoll_ctl(EPOLL_CTL_ADD, EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP)
  → insert into client_table[fd]

On EPOLLIN for client_fd:
  → handle subscription message (if not yet subscribed)
  → read and discard (server does not expect data after subscription)

On EPOLLOUT for client_fd:
  → flush pending send queue for this client
  → if queue empty, remove EPOLLOUT from interest set

On EPOLLERR | EPOLLHUP:
  → handle_client_disconnect(fd)
```

**Level-triggered mode** is used for client send sockets. The server's primary concern is writing data to clients; LT ensures `EPOLLOUT` keeps firing until the send buffer is fully drained, which simplifies the send loop.

```cpp
// Broadcast to all clients — called by broadcast_thread
void broadcast_message(const void* data, size_t len) {
    for (auto& [fd, state] : client_table_) {
        if (!state.pending_sends.empty()) {
            // Already backpressured — enqueue without trying
            state.pending_sends.emplace_back(data, data + len);
            check_slow_client(fd, state);
            continue;
        }
        ssize_t sent = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(len)) {
            state.bytes_sent += sent;
        } else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full — queue message and arm EPOLLOUT
            state.pending_sends.emplace_back(data, data + len);
            epoll_event ev{EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP, {.fd = fd}};
            epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
        } else if (sent == -1) {
            handle_client_disconnect(fd);
        }
    }
}
```

### 1b. Broadcast Strategy

**Direct iteration (chosen):** The broadcast_thread iterates the client table and calls `send()` for each connected fd. For up to a few hundred clients, this is O(N) and cache-friendly since `client_table_` is a flat hash map.

An alternative — a **dedicated per-client write thread** — was considered and rejected: thread creation/context switching overhead would dominate at high tick rates, and synchronisation between the tick generator and N writer threads would require N queues.

For 1000+ clients, the preferred architecture would switch to **io_uring** with fixed-size registered buffers, allowing the kernel to fan out writes without repeated user-space syscall overhead. This is noted as a future scalability path.

### 1c. Slow Client Detection and Handling

A client is flagged as slow when:

1. Its per-client `pending_sends` queue exceeds **64 MB**, OR
2. Its last successful receive acknowledgement is more than **5 seconds** old (detected via heartbeat mechanism).

Actions taken:
- **Soft warning (queue 16–64 MB):** Log a warning with the client's IP:port and queue depth.
- **Hard disconnect (queue > 64 MB):** Close the fd, remove from epoll, destroy `ClientState`. The client is expected to reconnect and resume from the current sequence number.

```cpp
void check_slow_client(int fd, ClientState& state) {
    size_t queued = 0;
    for (auto& buf : state.pending_sends) queued += buf.size();
    if (queued > SLOW_CLIENT_HARD_LIMIT) {
        log_warn("Dropping slow client fd={} queued={}MB", fd, queued >> 20);
        handle_client_disconnect(fd);
    }
}
```

### 1d. Connection State Management

Each connected client maintains a simple state machine:

```
ACCEPTED → SUBSCRIBED → LIVE → DISCONNECTED
```

- **ACCEPTED:** Connection received, socket configured. Waiting for a subscription message (type `0xFF`).
- **SUBSCRIBED:** Symbol list parsed. Client will now receive broadcasts for requested symbols (or all symbols if subscription count is zero).
- **LIVE:** Normal operation — receiving tick broadcasts.
- **DISCONNECTED:** `close(fd)` called, resources freed.

---

## 2. Client-Side Design

### 2a. Socket Programming Decisions

```cpp
// Key socket options set on connect:
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,     &one,   sizeof(one));
setsockopt(fd, SOL_SOCKET,  SO_RCVBUF,       &rcvbuf, sizeof(rcvbuf)); // 4 MB
setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,    &one,   sizeof(one));
setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,    &idle,  sizeof(idle));    // 10 s
setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,   &intvl, sizeof(intvl));   // 5 s
setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,     &cnt,   sizeof(cnt));     // 3 probes
fcntl(fd, F_SETFL, O_NONBLOCK);
```

`SO_PRIORITY` is set to `6` (highest non-root priority) to request preferential kernel scheduling for this socket's packets. Note: effective only if the NIC and kernel scheduler cooperate.

### 2b. Why epoll Over select/poll?

| Feature | `select` | `poll` | `epoll` |
|---------|---------|--------|--------|
| Scalability | O(fd_max) scan | O(N) scan | O(1) per event |
| Max fds | 1024 (FD_SETSIZE) | Unlimited | Unlimited |
| Kernel copies | Both ways per call | Both ways per call | Registered once |
| Edge-triggered | No | No | Yes |
| Linux kernel support | Universal | Universal | Linux 2.5.44+ |

For a single client socket, `select` vs `epoll` makes little practical difference. However, `epoll` is used here for two reasons:
1. **Consistency:** The same pattern is used in the server; using epoll everywhere reduces cognitive overhead.
2. **Extensibility:** If the client needs to monitor a timer fd (for heartbeats) or a signal fd alongside the TCP socket, epoll handles multiple fd types uniformly.

### 2c. Edge-Triggered vs Level-Triggered

**Edge-triggered (ET)** is chosen for the client's receive socket.

**Why ET for the client:**
- The client has a single socket to read. When `EPOLLIN` fires, the network thread **must drain the socket completely** (loop `recv()` until `EAGAIN`), which is the correct behaviour anyway — we want to empty the kernel buffer as fast as possible.
- ET eliminates spurious re-wakeups. With LT, epoll would continuously signal `EPOLLIN` while unread data remains, even if the application thread is already draining it. In a tight loop with a high-rate feed (100K msg/s), this wastes CPU.
- The downside — if the application forgets to drain completely it will miss data — is a known requirement and is enforced in code with a `do { ... } while (ret > 0)` read loop.

```cpp
// Edge-triggered drain loop — MUST drain to EAGAIN
while (true) {
    ssize_t n = recv(sock_fd_, buf_.write_ptr(), buf_.available(), 0);
    if (n > 0) {
        buf_.advance_write(n);
        total_bytes_ += n;
    } else if (n == 0) {
        // Peer closed connection (FIN received)
        on_disconnect(DisconnectReason::PEER_CLOSED);
        break;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // fully drained
        if (errno == EINTR) continue;                         // signal interrupted
        on_disconnect(DisconnectReason::RECV_ERROR);
        break;
    }
}
```

### 2d. Non-Blocking I/O Patterns

The client's epoll loop processes events as follows:

```
epoll_wait(epfd_, events, MAX_EVENTS, timeout_ms)
  → EPOLLIN:  drain recv buffer (ET loop above)
  → EPOLLERR: log errno from getsockopt(SO_ERROR), trigger reconnect
  → EPOLLHUP: peer closed — trigger reconnect
  → EPOLLRDHUP: half-close detected — trigger reconnect

Timer fd (CLOCK_MONOTONIC):
  → Heartbeat check: if last_recv_time > 30s, trigger reconnect
  → Stats snapshot: every 1s, post stats to visualizer
```

---

## 3. TCP Stream Handling

### 3a. Message Boundary Detection

TCP is a **byte stream protocol** — it provides no message framing. A single `recv()` call may return:
- **Less than one message** (partial read — header not yet complete).
- **Exactly one message**.
- **Multiple messages** concatenated.
- **A message split across a packet boundary**.

The parser handles this with a ring buffer and a simple state machine:

```
State: READING_HEADER (need 16 bytes)
  → If ring buffer has < 16 bytes: wait for more data
  → Once 16 bytes available: decode type, seq, timestamp, symbol_id
  → Compute expected payload size from type

State: READING_PAYLOAD (need 0–24 bytes depending on type)
  → If ring buffer has < payload_size bytes: wait for more data
  → Once complete: validate checksum, dispatch to cache update
  → Advance ring buffer read pointer
  → Return to READING_HEADER
```

### 3b. Partial Read Buffering Strategy

The `RecvBuffer` is a **power-of-two ring buffer** of 8 MB:

```cpp
struct RecvBuffer {
    alignas(64) uint8_t  data[8 * 1024 * 1024];
    alignas(64) uint64_t write_pos{0};   // written by network_thread
    alignas(64) uint64_t read_pos{0};    // read by parse_thread
    // Separate cache lines prevent false sharing between threads
};
```

`write_ptr()` returns `data + (write_pos & MASK)`. Since the buffer is power-of-two, the modulo is a single AND instruction.

**Wrap-around handling:** If a message straddles the buffer boundary (tail of the buffer wraps to the start), the parser copies it into a small local `scratch[64]` buffer before parsing — this is the only copy in the hot path beyond the initial `recv()`.

### 3c. Buffer Sizing Calculations

At 100K messages/second, with an average message size of ~35 bytes:

```
Incoming bandwidth ≈ 100,000 × 35 = 3.5 MB/s

Kernel receive buffer (SO_RCVBUF = 4 MB):
  At 3.5 MB/s, the buffer can absorb ~1.14 s of data before overflow.
  This gives the parse thread ample time to drain without dropping.

Application ring buffer (8 MB):
  Provides ~2.3 s of slack at 3.5 MB/s.
  In practice the parse thread is faster than the network thread; the buffer
  rarely exceeds a few KB of occupancy.

At maximum rate (500K msg/s × 35 bytes = 17.5 MB/s):
  Kernel buffer absorbed in ~230 ms.
  Application buffer absorbed in ~457 ms.
  At this rate the parse thread must sustain >500K parses/s to keep up.
```

---

## 4. Connection Management

### 4a. Connection State Machine

```
       ┌──────────────────────────────────┐
       │                                  │
       ▼                                  │
  DISCONNECTED ──connect()──► CONNECTING  │
                                   │      │
                    connect() ok   │      │
                                   ▼      │
                               CONNECTED  │
                                   │      │
               send_subscription() │      │
                                   ▼      │
                             SUBSCRIBING  │
                                   │      │
                  ACK from server  │      │
                                   ▼      │
                                 LIVE ────┘
                            (any error /
                             FIN / timeout)
```

State transitions are driven by the epoll event loop in `network_thread`. The `reconnect_thread` is a separate lightweight thread that sleeps for the backoff duration and then calls `connect()` again, posting the result back to the epoll loop via a `pipe` or `eventfd`.

### 4b. Retry Logic and Backoff Algorithm

```cpp
class ReconnectPolicy {
    int attempt_ = 0;
    static constexpr int BASE_MS  = 100;
    static constexpr int MAX_MS   = 30'000;

public:
    int next_delay_ms() {
        int delay = std::min(BASE_MS * (1 << attempt_), MAX_MS);
        delay += rand() % 100;  // jitter: 0-100 ms
        ++attempt_;
        return delay;
    }
    void reset() { attempt_ = 0; }
};
```

After 10 consecutive failures (backoff reaches 30 s), the handler emits a `FATAL` log entry and stops trying, requiring operator intervention. This avoids thundering-herd reconnection storms if the exchange simulator restarts.

### 4c. Heartbeat Mechanism

The server sends a `Heartbeat (0x03)` message every **1 second** if no other message has been sent in that window. This ensures:
1. The client can detect a **silent connection drop** (no FIN/RST, no data) by checking `last_recv_time`.
2. The TCP keepalive (`TCP_KEEPIDLE = 10s`) provides a lower-level backup.

The client's timer fd fires every 5 seconds to check:
```cpp
if (now - last_recv_time_ > 30s) {
    log_warn("Heartbeat timeout — reconnecting");
    on_disconnect(DisconnectReason::HEARTBEAT_TIMEOUT);
}
```

---

## 5. Error Handling

### 5a. Network Errors

| errno | Cause | Action |
|-------|-------|--------|
| `EAGAIN` / `EWOULDBLOCK` | No data in kernel buffer (ET: fully drained) | Break recv loop — expected |
| `EINTR` | Signal interrupted syscall | Retry recv |
| `ECONNRESET` | Server forcibly closed connection | Trigger reconnect |
| `EPIPE` / `ECONNREFUSED` | Server not reachable | Trigger reconnect with backoff |
| `ETIMEDOUT` | TCP retransmit exhausted | Trigger reconnect |
| `EMFILE` / `ENFILE` | File descriptor limit reached | Fatal — log and exit |

`SIGPIPE` is suppressed globally with `signal(SIGPIPE, SIG_IGN)`. All `send()` calls use the `MSG_NOSIGNAL` flag as an additional safeguard.

### 5b. Application-Level Errors

| Condition | Detection | Action |
|-----------|-----------|--------|
| Sequence gap | `seq != expected_seq` | Log gap size, increment `gap_counter`, continue |
| Bad checksum | XOR mismatch | Discard message, increment `checksum_error_counter` |
| Unknown message type | Type not in {0x01, 0x02, 0x03} | Skip payload bytes (computed from a safe max), continue |
| Message too large | Length field > `MAX_MSG_SIZE (128 bytes)` | Disconnect — likely a protocol error or attack |
| Out-of-order symbol ID | `symbol_id >= MAX_SYMBOLS (500)` | Discard message silently |

### 5c. Recovery Strategies

- **Sequence gaps:** Market data feeds conventionally do not support retransmission (the data is stale by the time a retransmit would arrive). The client logs the gap and continues. Downstream strategies must handle potentially missing ticks.
- **Checksum errors:** A single corrupted message is discarded. If the error rate exceeds 0.1% over a 10-second window, the client disconnects and reconnects (potential protocol de-sync).
- **Protocol de-sync:** If the parser's READING_HEADER state has waited more than 1 second for the next complete header (implies severe fragmentation or corruption), the parser resets its state and discards buffered bytes until it finds a valid header signature.
