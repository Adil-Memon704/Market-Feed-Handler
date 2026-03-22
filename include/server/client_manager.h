#pragma once

#include <cstdint>
#include <unordered_map>
#include <deque>
#include <vector>
#include <chrono>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Per-client state maintained by the server.
// ─────────────────────────────────────────────────────────────────────────────

enum class ClientPhase : uint8_t {
    ACCEPTED,
    SUBSCRIBED,
    LIVE,
};

struct ClientState {
    int         fd;
    ClientPhase phase     = ClientPhase::ACCEPTED;
    uint64_t    bytes_sent   = 0;
    uint64_t    msgs_sent    = 0;
    uint64_t    msgs_dropped = 0;

    // Pending send queue (for backpressure / slow clients)
    std::deque<std::vector<uint8_t>> pending;
    size_t pending_bytes = 0;

    // Subscription filter (empty = receive all)
    std::vector<uint16_t> subscribed_symbols;
    bool subscribe_all = true;

    // Slow-client detection
    std::chrono::steady_clock::time_point connected_at;
    std::chrono::steady_clock::time_point last_send;

    // Partially received subscription message
    std::vector<uint8_t> recv_buf;

    std::string peer_addr;  // "ip:port" for logging
};

// ─────────────────────────────────────────────────────────────────────────────
// ClientManager — manages the table of connected clients.
// Provides send/broadcast helpers used by the broadcast_thread.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t SLOW_CLIENT_WARN_BYTES = 16 * 1024 * 1024;   // 16 MB
static constexpr size_t SLOW_CLIENT_DROP_BYTES = 64 * 1024 * 1024;   // 64 MB

class ClientManager {
public:
    explicit ClientManager(int epfd);

    // Called from accept_thread
    void add_client(int fd, const std::string& peer_addr);

    // Called from broadcast_thread / epoll loop
    void remove_client(int fd);

    // Try to send data to one client. Returns false if client was dropped.
    bool send_to(int fd, const void* data, size_t len);

    // Broadcast to all LIVE clients
    void broadcast(const void* data, size_t len);

    // Flush pending queue for a client whose EPOLLOUT fired
    void flush_pending(int fd);

    // Handle incoming bytes from a client (subscription parsing)
    void on_recv(int fd, const void* data, size_t len);

    // Periodic maintenance (slow-client check, logging)
    void tick(std::chrono::steady_clock::time_point now);

    size_t client_count() const { return clients_.size(); }

    // Stats
    uint64_t total_msgs_sent()    const { return total_msgs_sent_; }
    uint64_t total_msgs_dropped() const { return total_msgs_dropped_; }

private:
    void enqueue(ClientState& cs, const void* data, size_t len);
    bool flush_queue(ClientState& cs);
    void arm_epollout(int fd, bool enable);

    int epfd_;
    std::unordered_map<int, ClientState> clients_;
    uint64_t total_msgs_sent_    = 0;
    uint64_t total_msgs_dropped_ = 0;
};
