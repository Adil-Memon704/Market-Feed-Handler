#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// DisconnectReason
// ─────────────────────────────────────────────────────────────────────────────

enum class DisconnectReason {
    PEER_CLOSED,
    RECV_ERROR,
    SEND_ERROR,
    HEARTBEAT_TIMEOUT,
    PROTOCOL_ERROR,
    USER_REQUESTED,
};

const char* disconnect_reason_str(DisconnectReason r);

// ─────────────────────────────────────────────────────────────────────────────
// ReconnectPolicy — exponential backoff with jitter.
// ─────────────────────────────────────────────────────────────────────────────

class ReconnectPolicy {
public:
    static constexpr int BASE_MS    = 100;
    static constexpr int MAX_MS     = 30'000;
    static constexpr int MAX_ATTEMPT = 20;   // give up after this many

    int  next_delay_ms();
    void reset()             { attempt_ = 0; }
    int  attempt()     const { return attempt_; }
    bool give_up()     const { return attempt_ >= MAX_ATTEMPT; }

private:
    int attempt_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MarketDataSocket — non-blocking TCP client.
// ─────────────────────────────────────────────────────────────────────────────

class MarketDataSocket {
public:
    MarketDataSocket();
    ~MarketDataSocket();

    // Attempt connection. Returns true if connected (or connection in progress).
    bool connect(const std::string& host, uint16_t port,
                 uint32_t timeout_ms = 5000);

    // Non-blocking receive. Returns bytes read, 0 on EOF, -1 on error.
    // errno == EAGAIN means no data available.
    ssize_t receive(void* buffer, size_t max_len);

    // Send subscription message.
    bool send_subscription(const std::vector<uint16_t>& symbol_ids);

    // Connection state
    bool is_connected() const;
    void disconnect();

    // Low-latency socket options
    bool set_tcp_nodelay(bool enable);
    bool set_recv_buffer_size(size_t bytes);
    bool set_socket_priority(int priority);

    int fd() const { return fd_; }

    // Retrieve and clear last system error
    int  last_error() const { return last_errno_; }

private:
    bool configure_socket();

    int  fd_         = -1;
    bool connected_  = false;
    int  last_errno_ = 0;
};
